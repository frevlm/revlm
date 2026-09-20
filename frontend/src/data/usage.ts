import { useQuery } from '@tanstack/react-query';

import {
  getUsageEventDetail,
  getUsageEvents,
  getUsageTimeSeries,
  getUsageWindows,
  type UsageEvent,
  type UsageEventDetail,
  type UsageTimeSeriesPoint,
  type UsageWindow,
} from '../api/usage';
import { fillDailyBuckets } from '../utils/timeSeries';
import { unwrap } from './unwrap';

export type UsageGranularity = 'hour' | 'day';

export type UsageSeries = {
  start: string;
  end: string;
  points: UsageTimeSeriesPoint[];
};

/**
 * The date range scoping the account's usage summary. `allTime` only actually
 * takes effect once start/end are both cleared — matching the "全部时间" toggle,
 * which is otherwise indistinguishable from a range the caller forgot to fill in.
 */
export type UsageWindowFilters = { start: string; end: string; allTime: boolean };

export type UsageEventsFilters = {
  start: string;
  end: string;
  allTime: boolean;
  limit: number;
  beforeID?: number;
  filterKey: string;
  filterModel: string;
};

export type UsageEventsPage = {
  events: UsageEvent[];
  next_before_id: number | null;
};

export const usageKeys = {
  timeSeries: (granularity: UsageGranularity) => ['usage', 'timeseries', granularity] as const,
  window: (filters: UsageWindowFilters) => ['usage', 'window', filters] as const,
  events: (filters: UsageEventsFilters) => ['usage', 'events', filters] as const,
  eventDetail: (eventID: number) => ['usage', 'events', eventID, 'detail'] as const,
};

/**
 * A time series over the caller's whole visible range at the given granularity.
 *
 * Daily series arrive with gaps — the backend emits a bucket only for days that
 * saw traffic — so they are made contiguous here rather than at each chart. The
 * filled series is what lands in the cache, so every reader of this key sees the
 * same shape without re-deriving it.
 */
export function useUsageTimeSeries(granularity: UsageGranularity, options?: { enabled?: boolean }) {
  return useQuery<UsageSeries>({
    queryKey: usageKeys.timeSeries(granularity),
    enabled: options?.enabled ?? true,
    queryFn: async () => {
      const data = await unwrap(getUsageTimeSeries(undefined, undefined, granularity), '时间序列加载失败');
      const start = data.start || '';
      const end = data.end || '';
      const points = data.points || [];
      return {
        start,
        end,
        points:
          granularity === 'day'
            ? fillDailyBuckets(points, start, end, (bucket) => ({
                bucket,
                requests: 0,
                usd: 0,
                avg_first_token_latency: 0,
              }))
            : points,
      };
    },
  });
}

function isAllTimeActive(filters: { start: string; end: string; allTime: boolean }): boolean {
  return filters.allTime && !filters.start.trim() && !filters.end.trim();
}

/** The account-level summary window (总请求数/消耗/RPM/首字延迟) for a date range. */
export function useUsageWindow(filters: UsageWindowFilters) {
  return useQuery<UsageWindow | null>({
    queryKey: usageKeys.window(filters),
    queryFn: async () => {
      const start = filters.start.trim();
      const end = filters.end.trim();
      const data = await unwrap(
        getUsageWindows(start || undefined, end || undefined, undefined, isAllTimeActive(filters)),
        '加载失败'
      );
      return data.windows?.[0] ?? null;
    },
  });
}

/**
 * One page of request events. `beforeID` is a keyset cursor rather than an
 * offset, so paging is a plain query keyed on the cursor — the page owns a
 * stack of visited cursors to support "上一页", the same way `beforeID` itself
 * is UI state that happens to feed this query's key.
 */
export function useUsageEvents(filters: UsageEventsFilters) {
  return useQuery<UsageEventsPage>({
    queryKey: usageKeys.events(filters),
    queryFn: async () => {
      const qKey = filters.filterKey.trim();
      const qModel = filters.filterModel.trim();
      const indexParts = [qKey ? 'key' : '', qModel ? 'model' : ''].filter(Boolean);
      const allTimeActive = isAllTimeActive(filters);
      const start = filters.start.trim();
      const end = filters.end.trim();
      const data = await unwrap(
        getUsageEvents({
          limit: filters.limit,
          before_id: filters.beforeID,
          start: allTimeActive ? undefined : start || undefined,
          end: allTimeActive ? undefined : end || undefined,
          index: indexParts.length ? indexParts.join(',') : undefined,
          q_key: qKey || undefined,
          q_model: qModel || undefined,
        }),
        '加载失败'
      );
      return { events: data.events || [], next_before_id: data.next_before_id ?? null };
    },
  });
}

/** A single event's `usage_details` payload, fetched only once its row is expanded. */
export function useUsageEventDetail(eventID: number | null) {
  return useQuery<UsageEventDetail>({
    queryKey: usageKeys.eventDetail(eventID ?? 0),
    enabled: eventID !== null,
    queryFn: () => unwrap(getUsageEventDetail(eventID as number), '加载详情失败'),
  });
}
