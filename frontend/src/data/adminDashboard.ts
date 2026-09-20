import { useQuery } from '@tanstack/react-query';

import { getAdminDashboard, type AdminDashboard } from '../api/admin/dashboard';
import { getAdminUsageTimeSeries, type AdminUsageTimeSeriesPoint } from '../api/admin/usage';
import { fillDailyBuckets } from '../utils/timeSeries';
import { unwrap } from './unwrap';

export const adminDashboardKeys = {
  all: ['adminDashboard'] as const,
};

export function useAdminDashboard() {
  return useQuery<AdminDashboard>({
    queryKey: adminDashboardKeys.all,
    queryFn: () => unwrap(getAdminDashboard(), '加载失败'),
  });
}

export const adminUsageSeriesKeys = {
  all: ['adminUsageSeries'] as const,
  granularity: (granularity: 'hour' | 'day') => ['adminUsageSeries', granularity] as const,
};

export type AdminUsageSeries = {
  start: string;
  end: string;
  points: AdminUsageTimeSeriesPoint[];
  isPending: boolean;
  error: Error | null;
};

/**
 * Unlike the user-facing dashboard, the admin overview has no embedded hourly
 * series — both granularities come from `/api/admin/request/timeseries`. Daily
 * buckets arrive with gaps, filled the same way as the account and channel
 * series in `data/usage.ts` / `data/channels.ts`.
 */
export function useAdminUsageSeries(granularity: 'hour' | 'day'): AdminUsageSeries {
  const query = useQuery({
    queryKey: adminUsageSeriesKeys.granularity(granularity),
    queryFn: async () => {
      const data = await unwrap(getAdminUsageTimeSeries({ granularity }), '加载时间序列失败');
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

  return {
    start: query.data?.start ?? '',
    end: query.data?.end ?? '',
    points: query.data?.points ?? [],
    isPending: query.isPending,
    error: query.error,
  };
}
