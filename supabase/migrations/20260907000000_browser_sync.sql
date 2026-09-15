create table if not exists public.browser_sync (
  user_id uuid not null references auth.users(id) on delete cascade,
  profile_id uuid not null,
  revision bigint not null default 1 check (revision > 0),
  payload text not null check (octet_length(payload) <= 10485760),
  updated_at timestamptz not null default now(),
  primary key (user_id, profile_id)
);

alter table public.browser_sync enable row level security;
revoke all on table public.browser_sync from anon, authenticated;
grant select, insert, update, delete on table public.browser_sync to authenticated;

create policy "browser sync select own"
on public.browser_sync for select to authenticated
using ((select auth.uid()) = user_id);

create policy "browser sync insert own"
on public.browser_sync for insert to authenticated
with check ((select auth.uid()) = user_id);

create policy "browser sync update own"
on public.browser_sync for update to authenticated
using ((select auth.uid()) = user_id)
with check ((select auth.uid()) = user_id);

create policy "browser sync delete own"
on public.browser_sync for delete to authenticated
using ((select auth.uid()) = user_id);

create or replace function public.push_browser_sync(
  p_profile_id uuid,
  p_expected_revision bigint,
  p_payload text
)
returns public.browser_sync
language plpgsql
security invoker
set search_path = ''
as $$
declare
  result public.browser_sync;
begin
  if (select auth.uid()) is null then
    raise exception 'authentication required' using errcode = '42501';
  end if;

  insert into public.browser_sync (user_id, profile_id, revision, payload)
  values ((select auth.uid()), p_profile_id, 1, p_payload)
  on conflict (user_id, profile_id) do update
    set revision = public.browser_sync.revision + 1,
        payload = excluded.payload,
        updated_at = now()
    where p_expected_revision is not null
      and public.browser_sync.revision = p_expected_revision
  returning * into result;

  if result is null then
    raise exception 'sync conflict' using errcode = '40001';
  end if;
  return result;
end;
$$;

revoke all on function public.push_browser_sync(uuid, bigint, text) from public, anon;
grant execute on function public.push_browser_sync(uuid, bigint, text) to authenticated;
