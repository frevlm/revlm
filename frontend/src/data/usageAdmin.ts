import { useQuery } from '@tanstack/react-query';

import {
  getAdminUsageEventDetail,
  getAdminUsagePage,
  type AdminUsageEvent,
  type AdminUsageUser,
  type AdminUsageWindow,
  type UsageEventDetail,
} from '../api/admin/usage';
import { unwrap } from './unwrap';

/**
 * The full filter set for the admin usage page, minus pagination. Kept apart
 * from the cursor (see `AdminUsageCursor`) because the two vary independently:
 * a filter change starts the listing over, a cursor change does not.
 */
export type AdminUsageFilters = {
  start: string;
  end: string;
  allTime: boolean;
  limit: number;
  filterUser: string;
  filterUserID?: number;
  filterChannel: string;
  filterChannelID?: number;
  filterModel: string;
  filterModelExact?: string;
};

export type AdminUsageCursor = { beforeID?: number; afterID?: number };

export type AdminUsageEventsPage = {
  events: AdminUsageEvent[];
  next_before_id?: number;
  prev_after_id?: number;
};

export const usageAdminKeys = {
  summary: (filters: AdminUsageFilters) => ['usageAdmin', 'summary', filters] as const,
  events: (filters: AdminUsageFilters, cursor: AdminUsageCursor) => ['usageAdmin', 'events', filters, cursor] as const,
  eventDetail: (eventID: number) => ['usageAdmin', 'events', eventID, 'detail'] as const,
};

/**
 * Builds the `/api/admin/request` query params shared by the summary and
 * cursor requests below. A cursor is only ever attached by the events query —
 * its presence is also what tells the backend to skip recomputing
 * window/top_users (`summary: false`), which is why the summary request below
 * never passes one.
 */
function buildAdminUsageParams(filters: AdminUsageFilters, cursor?: AdminUsageCursor) {
  const start = filters.start.trim();
  const end = filters.end.trim();
  const allTimeActive = filters.allTime && !start && !end;
  const qUser = filters.filterUser.trim();
  const qChannel = filters.filterChannel.trim();
  const qModel = filters.filterModel.trim();
  const indexParts = [
    !filters.filterUserID && qUser ? 'user' : '',
    !filters.filterChannelID && qChannel ? 'channel' : '',
    !filters.filterModelExact && qModel ? 'model' : '',
  ].filter(Boolean);

  const params: Parameters<typeof getAdminUsagePage>[0] = {
    limit: filters.limit,
    index: indexParts.length ? indexParts.join(',') : undefined,
    user_id: filters.filterUserID,
    channel_id: filters.filterChannelID,
    model: filters.filterModelExact,
    q_user: !filters.filterUserID ? qUser || undefined : undefined,
    q_channel: !filters.filterChannelID ? qChannel || undefined : undefined,
    q_model: !filters.filterModelExact ? qModel || undefined : undefined,
    ...(allTimeActive ? { all_time: true } : { start: start || undefined, end: end || undefined }),
  };

  if (cursor?.beforeID) params.before_id = cursor.beforeID;
  if (cursor?.afterID) params.after_id = cursor.afterID;
  if (cursor?.beforeID || cursor?.afterID) params.summary = false;

  return params;
}

export type AdminUsageSummary = {
  window?: AdminUsageWindow;
  top_users: AdminUsageUser[];
  events: AdminUsageEvent[];
  next_before_id?: number;
  prev_after_id?: number;
};

/**
 * The window stats, top-users table and first page of events for the current
 * filters. This is the only query that ever asks for window/top_users, so
 * paging (see `useAdminUsageEvents`) never has to re-fetch or patch them back
 * in — they simply stay cached under this key while only the events query
 * below refetches.
 */
export function useAdminUsageSummary(filters: AdminUsageFilters) {
  return useQuery<AdminUsageSummary>({
    queryKey: usageAdminKeys.summary(filters),
    queryFn: async () => {
      const data = await unwrap(getAdminUsagePage(buildAdminUsageParams(filters)), '加载失败');
      return {
        window: data.window,
        top_users: data.top_users || [],
        events: data.events || [],
        next_before_id: data.next_before_id,
        prev_after_id: data.prev_after_id,
      };
    },
  });
}

/**
 * A page of events at a given cursor. Disabled until a cursor is actually set
 * ("上一页"/"下一页" clicked) — the first page is served by `useAdminUsageSummary`
 * instead, so this query never duplicates that request.
 */
export function useAdminUsageEvents(filters: AdminUsageFilters, cursor: AdminUsageCursor) {
  return useQuery<AdminUsageEventsPage>({
    queryKey: usageAdminKeys.events(filters, cursor),
    enabled: cursor.beforeID !== undefined || cursor.afterID !== undefined,
    queryFn: async () => {
      const data = await unwrap(getAdminUsagePage(buildAdminUsageParams(filters, cursor)), '加载失败');
      return { events: data.events || [], next_before_id: data.next_before_id, prev_after_id: data.prev_after_id };
    },
  });
}

/** A single event's `usage_details` payload, fetched only once its row is expanded. */
export function useAdminUsageEventDetail(eventID: number | null) {
  return useQuery<UsageEventDetail>({
    queryKey: usageAdminKeys.eventDetail(eventID ?? 0),
    enabled: eventID !== null,
    queryFn: () => unwrap(getAdminUsageEventDetail(eventID as number), '加载详情失败'),
  });
}
