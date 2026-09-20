-- User-visible inventory and revocation for QR-paired devices.
alter table public.browser_devices add column if not exists label text not null default 'iPhone';
alter table public.browser_devices add column if not exists platform text not null default 'ios'
  check (platform in ('ios', 'macos'));

create or replace function public.redeem_browser_pairing(
  p_pairing_secret text, p_device_hash text, p_label text default 'iPhone', p_platform text default 'ios'
)
returns uuid language plpgsql security definer set search_path = '' as $$
declare pair public.browser_pairings;
begin
  select * into pair from public.browser_pairings
    where pairing_hash = encode(digest(p_pairing_secret, 'sha256'), 'hex')
      and consumed_at is null and expires_at > now() for update;
  if pair is null or length(p_device_hash) <> 64 or p_platform not in ('ios', 'macos') then raise exception 'pairing expired'; end if;
  update public.browser_pairings set consumed_at = now() where pairing_hash = pair.pairing_hash;
  insert into public.browser_devices(device_hash, user_id, profile_id, label, platform)
    values (p_device_hash, pair.user_id, pair.profile_id, left(trim(p_label), 80), p_platform);
  return pair.profile_id;
end; $$;
grant execute on function public.redeem_browser_pairing(text, text, text, text) to anon, authenticated;

create or replace function public.list_browser_devices(p_profile_id uuid)
returns table(id text, label text, platform text, created_at timestamptz, last_used_at timestamptz)
language sql security definer set search_path = '' as $$
  select device_hash, label, platform, created_at, last_used_at
  from public.browser_devices
  where user_id = auth.uid() and profile_id = p_profile_id and revoked_at is null
  order by last_used_at desc;
$$;
grant execute on function public.list_browser_devices(uuid) to authenticated;

create or replace function public.revoke_browser_device(p_device_id text, p_profile_id uuid)
returns void language plpgsql security definer set search_path = '' as $$
begin
  update public.browser_devices set revoked_at = now()
  where device_hash = p_device_id
    and user_id = auth.uid() and profile_id = p_profile_id;
end; $$;
grant execute on function public.revoke_browser_device(text, uuid) to authenticated;
