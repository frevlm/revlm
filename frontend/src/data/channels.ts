import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import {
  createChannel,
  deleteChannel,
  getChannelsPage,
  getChannelTimeSeries,
  updateChannel,
  type ChannelItem,
  type ChannelTimeSeriesPoint,
  type CreateChannelRequest,
  type UpdateChannelRequest,
} from '../api/channels';
import { fillDailyBuckets } from '../utils/timeSeries';
import { unwrap, unwrapOk } from './unwrap';

/**
 * The statistics window the channel table is scoped to. An empty start/end pair
 * means "let the server pick today's window", which is a different request from
 * `allTime` — hence three fields rather than two nullable ones.
 */
export type ChannelsRange = { start: string; end: string; allTime: boolean };

export type ChannelsPage = {
  start: string;
  end: string;
  channels: ChannelItem[];
};

export type ChannelSeriesParams = {
  start?: string;
  end?: string;
  allTime?: boolean;
  granularity: 'hour' | 'day';
};

export type ChannelSeries = {
  start: string;
  end: string;
  points: ChannelTimeSeriesPoint[];
};

export const channelKeys = {
  all: ['channels'] as const,
  page: (range: ChannelsRange) =>
    ['channels', 'page', range.allTime ? { allTime: true } : { start: range.start, end: range.end }] as const,
  timeSeries: (channelID: number, params: ChannelSeriesParams) =>
    ['channels', 'timeseries', channelID, params] as const,
};

export function useChannels(range: ChannelsRange) {
  return useQuery<ChannelsPage>({
    queryKey: channelKeys.page(range),
    // `placeholderData: keepPreviousData` is deliberately not used: the table's
    // numbers are scoped to the window, and showing the old window's figures
    // under a new window's label would be a lie.
    queryFn: async () => {
      const params = range.allTime
        ? { all_time: true }
        : { start: range.start.trim() || undefined, end: range.end.trim() || undefined };
      const data = await unwrap(getChannelsPage(params), '加载渠道失败');
      return {
        start: data.start || '',
        end: data.end || '',
        channels: data.channels || [],
      };
    },
  });
}

/**
 * One channel's usage series. Daily series arrive with gaps — only days that saw
 * traffic get a bucket — so they are made contiguous here, the same rule the
 * account-level series follows in `src/data/usage.ts`.
 */
export function useChannelTimeSeries(channelID: number | null, params: ChannelSeriesParams) {
  return useQuery<ChannelSeries>({
    queryKey: channelKeys.timeSeries(channelID ?? 0, params),
    enabled: channelID !== null,
    queryFn: async () => {
      const data = await unwrap(
        getChannelTimeSeries(channelID as number, {
          start: params.start,
          end: params.end,
          all_time: params.allTime ? true : undefined,
          granularity: params.granularity,
        }),
        '加载时间序列失败'
      );
      const start = data.start || '';
      const end = data.end || '';
      const points = data.points || [];
      return {
        start,
        end,
        points:
          params.granularity === 'day'
            ? fillDailyBuckets(points, start, end, (bucket) => ({
                bucket,
                usd: '0',
                avg_first_token_latency: '0',
              }))
            : points,
      };
    },
  });
}

/**
 * Every channel mutation invalidates the whole `channels` subtree rather than
 * patching cached rows in place. A channel's row carries derived fields the
 * client cannot compute — usage totals, runtime ban state, whether the channel
 * is in use — so a locally patched row would be right about the edited fields
 * and stale about the rest.
 */
function useChannelMutation<TVars>(run: (vars: TVars) => Promise<void>) {
  const client = useQueryClient();
  return useMutation({
    mutationFn: run,
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: channelKeys.all });
    },
  });
}

export function useCreateChannel() {
  const client = useQueryClient();
  return useMutation({
    mutationFn: (req: CreateChannelRequest) => unwrap(createChannel(req), '创建渠道失败'),
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: channelKeys.all });
    },
  });
}

export function useUpdateChannel() {
  return useChannelMutation((req: UpdateChannelRequest) => unwrapOk(updateChannel(req), '保存失败'));
}

export function useDeleteChannel() {
  return useChannelMutation((channelID: number) => unwrapOk(deleteChannel(channelID), '删除渠道失败'));
}
