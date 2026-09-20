import { useQuery } from '@tanstack/react-query';

import { getDashboard, type DashboardData } from '../api/dashboard';
import type { UsageTimeSeriesPoint } from '../api/usage';
import { unwrap } from './unwrap';
import { useUsageTimeSeries, type UsageGranularity } from './usage';

export const dashboardKeys = {
  all: ['dashboard'] as const,
};

export function useDashboard() {
  return useQuery<DashboardData>({
    queryKey: dashboardKeys.all,
    queryFn: () => unwrap(getDashboard(), '加载失败'),
  });
}

export type DashboardSeries = {
  start: string;
  end: string;
  points: UsageTimeSeriesPoint[];
  isPending: boolean;
  error: Error | null;
};

/**
 * The dashboard chart is served by two different endpoints: the hourly view
 * ships inside the `/api/dashboard` payload itself, the daily view comes from
 * `/api/request/timeseries`. Both are resolved here so the page sees one series
 * with one pending flag and one error, and never learns which endpoint answered.
 */
export function useDashboardSeries(granularity: UsageGranularity): DashboardSeries {
  const dashboard = useDashboard();
  const daily = useUsageTimeSeries('day', { enabled: granularity === 'day' });

  if (granularity === 'day') {
    return {
      start: daily.data?.start ?? '',
      end: daily.data?.end ?? '',
      points: daily.data?.points ?? [],
      isPending: daily.isPending,
      error: daily.error,
    };
  }
  return {
    start: dashboard.data?.today_since ?? '',
    end: dashboard.data?.today_until ?? '',
    points: dashboard.data?.charts.time_series_stats ?? [],
    isPending: dashboard.isPending,
    error: dashboard.error,
  };
}
