-- A phone never receives the owner's Supabase session.  It receives a
-- profile-scoped device capability after a short-lived QR bootstrap instead.
create extension if not exists pgcrypto;

create table public.browser_pairings (
  pairing_hash text primary key,
  user_id uuid not null references auth.users(id) on delete cascade,
  profile_id uuid not null,
  expires_at timestamptz not null,
  consumed_at timestamptz
);
create table public.browser_devices (
  device_hash text primary key,
  user_id uuid not null references auth.users(id) on delete cascade,
  profile_id uuid not null,
  created_at timestamptz not null default now(),
  last_used_at timestamptz not null default now(),
  revoked_at timestamptz
);
alter table public.browser_pairings enable row level security;
alter table public.browser_devices enable row level security;
revoke all on public.browser_pairings, public.browser_devices from public, anon, authenticated;

create or replace function public.create_browser_pairing(p_pairing_hash text, p_profile_id uuid)
returns void language plpgsql security definer set search_path = '' as $$
begin
  if auth.uid() is null or length(p_pairing_hash) <> 64 then raise exception 'invalid pairing'; end if;
  delete from public.browser_pairings where user_id = auth.uid() and profile_id = p_profile_id;
  insert into public.browser_pairings(pairing_hash, user_id, profile_id, expires_at)
    values (p_pairing_hash, auth.uid(), p_profile_id, now() + interval '10 minutes');
end; $$;
grant execute on function public.create_browser_pairing(text, uuid) to authenticated;

-- Called anonymously exactly once, over TLS, after scanning the QR code.  The
-- returned value is intentionally empty: the phone generated its own secret.
create or replace function public.redeem_browser_pairing(p_pairing_secret text, p_device_hash text)
returns uuid language plpgsql security definer set search_path = '' as $$
declare pair public.browser_pairings;
begin
  select * into pair from public.browser_pairings
    where pairing_hash = encode(digest(p_pairing_secret, 'sha256'), 'hex')
      and consumed_at is null and expires_at > now() for update;
  if pair is null or length(p_device_hash) <> 64 then raise exception 'pairing expired'; end if;
  update public.browser_pairings set consumed_at = now() where pairing_hash = pair.pairing_hash;
  insert into public.browser_devices(device_hash, user_id, profile_id)
    values (p_device_hash, pair.user_id, pair.profile_id);
  return pair.profile_id;
end; $$;
grant execute on function public.redeem_browser_pairing(text, text) to anon, authenticated;

-- Capability-scoped pull/push: it can access one encrypted profile only.
create or replace function public.paired_browser_sync(p_device_secret text, p_payload text default null, p_expected_revision bigint default null)
returns public.browser_sync language plpgsql security definer set search_path = '' as $$
declare device public.browser_devices; result public.browser_sync;
begin
  select * into device from public.browser_devices where device_hash = encode(digest(p_device_secret, 'sha256'), 'hex') and revoked_at is null for update;
  if device is null then raise exception 'device not paired' using errcode = '42501'; end if;
  update public.browser_devices set last_used_at = now() where device_hash = device.device_hash;
  if p_payload is null then
    select * into result from public.browser_sync where user_id = device.user_id and profile_id = device.profile_id;
  else
    insert into public.browser_sync(user_id, profile_id, revision, payload) values(device.user_id, device.profile_id, 1, p_payload)
    on conflict(user_id, profile_id) do update set revision = public.browser_sync.revision + 1, payload = excluded.payload, updated_at = now()
      where p_expected_revision is not null and public.browser_sync.revision = p_expected_revision
    returning * into result;
    if result is null then raise exception 'sync conflict' using errcode = '40001'; end if;
  end if;
  return result;
end; $$;
grant execute on function public.paired_browser_sync(text, text, bigint) to anon, authenticated;
