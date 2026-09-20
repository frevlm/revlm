import { Fragment, useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { useAuth } from '../../auth/AuthContext';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { formatSecondsFromMilliseconds } from '../../format/duration';
import { formatIntComma } from '../../format/int';
import type { Channel, ChannelItem, ChannelTimeSeriesPoint } from '../../api/channels';
import type { AdminChannelGroup } from '../../api/admin/channelGroups';
import {
  useChannels,
  useChannelTimeSeries,
  useDeleteChannel,
  useUpdateChannel,
  type ChannelsRange,
  type ChannelSeriesParams,
} from '../../data/channels';
import { useChannelGroups, useSetChannelGroupPointer } from '../../data/channelGroups';
import { useDebouncedValue } from '../../hooks/useDebouncedValue';
import { LineChart, type ChartTone } from '../../ui/LineChart';
import { Alert } from '../../ui/Alert';
import { Badge, type BadgeTone } from '../../ui/Badge';
import { Button } from '../../ui/Button';
import { Modal } from '../../ui/Modal';
import { Select } from '../../ui/Select';
import { Table } from '../../ui/Table';
import { DateRangePicker } from '../../components/DateRangePicker';
import { ChannelCommonTab } from './channels/ChannelCommonTab';
import { parseGroupsCSV } from './channels/utils';

function statusBadge(status: boolean): { tone: BadgeTone; label: string } {
  return status ? { tone: 'success', label: '启用' } : { tone: 'neutral', label: '禁用' };
}

type ChannelPointerTarget = {
  id: number;
  name: string;
  groups: string;
};

const emptyRange: ChannelsRange = { start: '', end: '', allTime: false };

type ChannelSeriesField = 'usd' | 'avg_first_token_latency';

const fieldOptions: Array<{ value: ChannelSeriesField; label: string }> = [
  { value: 'usd', label: '消耗 (USD)' },
  { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
];

// Money and latency travel as decimal strings — money never travels as a double.
const fieldMeta: Record<
  ChannelSeriesField,
  { label: string; tone: ChartTone; read: (point: ChannelTimeSeriesPoint) => number }
> = {
  usd: { label: '消耗 (USD)', tone: 'primary', read: (point) => Number(point.usd) || 0 },
  avg_first_token_latency: {
    label: '首字延迟 (s)',
    tone: 'danger',
    read: (point) => (Number(point.avg_first_token_latency) || 0) / 1000,
  },
};

const granularityOptions: Array<{ value: 'hour' | 'day'; label: string }> = [
  { value: 'hour', label: '按小时' },
  { value: 'day', label: '按天' },
];

export function ChannelsPage() {
  useAuth();
  const channelTableCols = 3;

  // `rangeInput` is what the picker shows; `range` is what the query is keyed
  // on. Typing a range therefore costs one request, not one per keystroke.
  const [rangeInput, setRangeInput] = useState<ChannelsRange>(emptyRange);
  const [rangeCustomized, setRangeCustomized] = useState(false);
  const range = useDebouncedValue(rangeInput, 400);

  const channelsQuery = useChannels(range);
  const channelGroupsQuery = useChannelGroups();
  const updateChannel = useUpdateChannel();
  const deleteChannel = useDeleteChannel();
  const setChannelGroupPointer = useSetChannelGroupPointer();

  const loading = channelsQuery.isPending;
  const channelGroups = useMemo(() => channelGroupsQuery.data ?? [], [channelGroupsQuery.data]);

  // Disabled channels are pinned below the enabled ones; the divider row keys
  // off the first disabled index.
  const channels = useMemo<ChannelItem[]>(() => {
    const list = channelsQuery.data?.channels ?? [];
    return [...list.filter((ch) => ch.status), ...list.filter((ch) => !ch.status)];
  }, [channelsQuery.data]);

  // The server echoes the window it actually used when the picker is empty.
  const rangeStartText = rangeInput.start || channelsQuery.data?.start || '';
  const rangeEndText = rangeInput.end || channelsQuery.data?.end || '';

  const [expandedChannelID, setExpandedChannelID] = useState<number | null>(null);
  const [pointerTarget, setPointerTarget] = useState<ChannelPointerTarget | null>(null);
  const [pointerGroupID, setPointerGroupID] = useState('');
  const [detailPanelByChannel, setDetailPanelByChannel] = useState<Record<number, 'stats' | 'accounts'>>({});
  const [detailField, setDetailField] = useState<ChannelSeriesField>('usd');
  const [detailGranularity, setDetailGranularity] = useState<'hour' | 'day'>('hour');
  const [creating, setCreating] = useState(false);
  const [settingsChannelID, setSettingsChannelID] = useState<number | null>(null);

  const oauthQueryHandled = useRef(false);

  // A daily series over the console's default window is wider than the table's
  // window, so an uncustomized daily view deliberately sends no range and lets
  // the server pick one.
  const seriesParams = useMemo<ChannelSeriesParams>(() => {
    const implicitDayRange = detailGranularity === 'day' && !rangeCustomized && !range.allTime;
    const omitRange = range.allTime || implicitDayRange;
    return {
      start: omitRange ? undefined : range.start.trim() || undefined,
      end: omitRange ? undefined : range.end.trim() || undefined,
      allTime: range.allTime || undefined,
      granularity: detailGranularity,
    };
  }, [detailGranularity, range, rangeCustomized]);

  const seriesQuery = useChannelTimeSeries(expandedChannelID, seriesParams);
  const detailSeries = useMemo(() => seriesQuery.data?.points ?? [], [seriesQuery.data]);
  const detailSeriesStart = seriesQuery.data?.start ?? '';
  const detailSeriesEnd = seriesQuery.data?.end ?? '';
  const detailSeriesLoading = expandedChannelID !== null && seriesQuery.isPending;
  const detailSeriesErr = seriesQuery.error ? seriesQuery.error.message : '';

  const enabledCount = useMemo(() => channels.filter((c) => c.status).length, [channels]);
  const disabledCount = channels.length - enabledCount;
  const firstDisabledIndex = useMemo(() => channels.findIndex((c) => !c.status), [channels]);

  // Derived, not stored: the settings modal edits a row the table already has.
  const settingsChannel = useMemo<Channel | null>(
    () => channels.find((ch) => ch.id === settingsChannelID) ?? null,
    [channels, settingsChannelID]
  );

  function toggleChannelPanel(channelID: number) {
    setExpandedChannelID((prev) => (prev === channelID ? null : channelID));
  }

  const channelGroupByName = useMemo(() => {
    const m = new Map<string, AdminChannelGroup>();
    for (const g of channelGroups) {
      const name = (g.name || '').trim();
      if (!name) continue;
      if (m.has(name)) continue;
      m.set(name, g);
    }
    return m;
  }, [channelGroups]);

  const pointerGroupOptions = useMemo(() => {
    if (!pointerTarget) return [];
    const names = parseGroupsCSV(pointerTarget.groups || '');
    const out: AdminChannelGroup[] = [];
    for (const name of names) {
      const g = channelGroupByName.get(name);
      if (!g || !g.status) continue;
      out.push(g);
    }
    return out;
  }, [pointerTarget, channelGroupByName]);

  const openChannelSettingsModal = useCallback((channelID: number) => {
    setSettingsChannelID(channelID);
  }, []);

  useEffect(() => {
    if (oauthQueryHandled.current || loading) return;
    if (typeof window === 'undefined') return;
    oauthQueryHandled.current = true;

    const params = new URLSearchParams(window.location.search);
    const openChannelSettings = Number.parseInt(params.get('open_channel_settings') || '', 10);
    const oauthState = (params.get('oauth') || '').trim();
    const oauthErr = (params.get('err') || '').trim();

    if (openChannelSettings > 0) {
      const target = channels.find((ch) => ch.id === openChannelSettings);
      if (target) openChannelSettingsModal(target.id);
    }

    if (openChannelSettings > 0 || oauthState !== '' || oauthErr !== '') {
      params.delete('open_channel_settings');
      params.delete('oauth');
      params.delete('err');
      const nextQuery = params.toString();
      const nextURL = `${window.location.pathname}${nextQuery ? `?${nextQuery}` : ''}${window.location.hash || ''}`;
      window.history.replaceState({}, '', nextURL);
    }
  }, [channels, loading, openChannelSettingsModal]);

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="d-flex justify-content-between align-items-start flex-wrap gap-3">
          <div>
            <h2 className="h4 fw-bold mb-1">上游渠道管理</h2>
            <p className="text-muted small mb-0">
              管理模型转发渠道。当前 {formatIntComma(enabledCount)} 启用 / {formatIntComma(disabledCount)} 禁用 /{' '}
              {formatIntComma(channels.length)} 总计。
            </p>
          </div>
          <Button variant="solid" tone="primary" disabled={creating} onClick={() => setCreating(true)}>
            <i className="ri-add-line me-1"></i>
            新建渠道
          </Button>
        </div>

        <div
          className="d-flex flex-wrap align-items-center gap-2 mb-0 bg-white p-2 rounded-3 border-light shadow-sm mt-3"
          style={{ border: '1px solid #f1f3f5' }}
        >
          <div className="d-flex align-items-center px-2">
            <span className="small text-muted me-2" style={{ whiteSpace: 'nowrap', fontSize: '12px' }}>
              统计区间
            </span>
            <DateRangePicker
              start={rangeStartText}
              end={rangeEndText}
              onChange={(r) => {
                const isAll = !r.start.trim() && !r.end.trim();
                setRangeCustomized(true);
                if (isAll) setDetailGranularity('day');
                setRangeInput({ start: r.start, end: r.end, allTime: isAll });
              }}
              loading={loading}
            />
          </div>

          <div className="ms-auto d-flex gap-2 pe-1">
            <Button
              variant="solid"
              tone="primary"
              size="sm"
              disabled={loading}
              onClick={() => {
                void channelsQuery.refetch();
              }}
            >
              <span className="material-symbols-rounded me-1" style={{ fontSize: '16px' }}>
                refresh
              </span>
              刷新数据
            </Button>
            <Button
              variant="soft"
              size="sm"
              disabled={loading}
              onClick={() => {
                setRangeCustomized(false);
                setRangeInput(emptyRange);
              }}
            >
              重置
            </Button>
          </div>
        </div>

        <div>
          <div className="card border-0 shadow-sm overflow-hidden mb-0">
            <div className="bg-primary bg-opacity-10 py-3 px-4 d-flex justify-content-between align-items-center">
              <div>
                <span className="text-primary fw-bold text-uppercase small">渠道列表</span>
              </div>
            </div>
            <Table
              head={
                <tr>
                  <th className="ps-4">渠道详情</th>
                  <th>状态</th>
                  <th className="text-end pe-4">操作</th>
                </tr>
              }
            >
              {loading ? (
                <tr>
                  <td colSpan={channelTableCols} className="text-center py-5 text-muted">
                    加载中…
                  </td>
                </tr>
              ) : channels.length === 0 ? (
                <tr>
                  <td colSpan={channelTableCols} className="text-center py-5 text-muted">
                    <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
                    暂无渠道。
                  </td>
                </tr>
              ) : (
                <>
                  {channels.map((ch, idx) => {
                    const st = statusBadge(ch.status);
                    const channelDisabled = !ch.status;
                    const runtime = ch.runtime;
                    const usage = ch.usage;
                    const panelOpen = expandedChannelID === ch.id;
                    const detailPanel = detailPanelByChannel[ch.id] || 'stats';
                    const rowBaseClassName = [
                      'rlm-channel-row-main',
                      channelDisabled ? 'table-secondary opacity-75' : '',
                    ]
                      .filter((v) => v)
                      .join(' ');
                    const groupNames = parseGroupsCSV(ch.groups || '');
                    const pointerGroups = groupNames
                      .map((name) => channelGroupByName.get(name))
                      .filter((g): g is AdminChannelGroup => !!g && g.status);
                    const canSetPointer = !channelDisabled && pointerGroups.length > 0;
                    const setPointerTitle = channelDisabled
                      ? '禁用渠道不可设为指针'
                      : pointerGroups.length === 0
                        ? '该渠道未加入任何启用的渠道组'
                        : '设为指针';

                    const renderMainRowCells = () => (
                      <>
                        <td className="ps-4" style={{ minWidth: 0 }}>
                          <div className="d-flex flex-column">
                            <div className="d-flex flex-wrap align-items-center gap-2">
                              <span className="fw-bold text-dark">{ch.name || `渠道 #${ch.id}`}</span>
                              <span className="text-muted small">({ch.type})</span>
                              {ch.in_use ? (
                                <span className="badge bg-info bg-opacity-10 text-info border border-info-subtle">
                                  使用中
                                </span>
                              ) : null}
                            </div>
                            <div className="d-flex flex-wrap align-items-center gap-2 small text-muted mt-1">
                              {ch.base_url ? (
                                <span
                                  className="font-monospace d-inline-block user-select-all"
                                  style={{
                                    maxWidth: 360,
                                    whiteSpace: 'nowrap',
                                    overflow: 'hidden',
                                    textOverflow: 'ellipsis',
                                  }}
                                  title={ch.base_url}
                                >
                                  {ch.base_url}
                                </span>
                              ) : null}
                              <div className="d-flex align-items-center">
                                {ch.base_url ? <span className="text-secondary">·</span> : null}
                                <span className={`${ch.base_url ? 'ms-2 ' : ''}me-1`}>渠道组:</span>
                                <span className="text-secondary font-monospace user-select-all">
                                  {(ch.groups || '').trim() || '-'}
                                </span>
                              </div>
                            </div>
                          </div>
                        </td>
                        <td>
                          <Badge tone={st.tone}>{st.label}</Badge>
                          {runtime?.available && runtime.banned_active ? (
                            <div className="mt-1">
                              <Badge
                                tone="warning"
                                title={runtime.banned_until ? `封禁至 ${runtime.banned_until}` : undefined}
                              >
                                <i className="ri-forbid-2-line me-1"></i>
                                封禁中 · 剩余 {runtime.banned_remaining || '-'}
                              </Badge>
                            </div>
                          ) : null}
                          {runtime?.available && typeof runtime.fail_score === 'number' && runtime.fail_score > 0 ? (
                            <div className="mt-1">
                              <Badge title="失败计分（运行态 fail score，越高越容易触发封禁/探测）">
                                失败计分：{runtime.fail_score}
                              </Badge>
                            </div>
                          ) : null}
                        </td>
                        <td className="text-end pe-4 text-nowrap">
                          <div className="d-flex gap-1 justify-content-end">
                            <Button
                              variant="soft"
                              tone={ch.status ? 'warning' : 'success'}
                              size="sm"
                              title={ch.status ? '禁用渠道' : '启用渠道'}
                              disabled={loading}
                              onClick={() => {
                                // Deliberately silent: the row reflects the
                                // result once the mutation invalidates the
                                // channel list, and a failed toggle leaves
                                // the old status showing.
                                updateChannel.mutate({ id: ch.id, status: !ch.status });
                              }}
                            >
                              <i className={`me-1 ${ch.status ? 'ri-pause-circle-line' : 'ri-play-circle-line'}`}></i>
                              {ch.status ? '禁用' : '启用'}
                            </Button>

                            <Button
                              variant="soft"
                              tone="warning"
                              size="sm"
                              title={setPointerTitle}
                              disabled={loading || !canSetPointer}
                              onClick={() => {
                                if (!canSetPointer) return;
                                setPointerTarget({
                                  id: ch.id,
                                  name: ch.name || `渠道 #${ch.id}`,
                                  groups: ch.groups || '',
                                });
                                setPointerGroupID(String(pointerGroups[0].id));
                              }}
                            >
                              <i className="ri-pushpin-2-line me-1"></i>指针
                            </Button>

                            <Button
                              variant="solid"
                              tone="primary"
                              size="sm"
                              title="设置"
                              disabled={loading}
                              onClick={() => openChannelSettingsModal(ch.id)}
                            >
                              <i className="ri-settings-3-line me-1"></i>
                              设置
                            </Button>

                            <Button
                              variant="soft"
                              tone="danger"
                              size="sm"
                              title="删除"
                              disabled={loading}
                              onClick={() => {
                                if (!window.confirm(`确认删除渠道 ${ch.name || ch.id} ? 此操作不可恢复。`)) return;
                                deleteChannel.mutate(ch.id);
                              }}
                            >
                              <i className="ri-delete-bin-line me-1"></i>
                              删除
                            </Button>
                          </div>
                        </td>
                      </>
                    );

                    return (
                      <Fragment key={ch.id}>
                        {idx === firstDisabledIndex ? (
                          <tr className="table-light">
                            <td colSpan={channelTableCols} className="px-4 py-2">
                              <span className="text-muted small">
                                <i className="ri-forbid-2-line me-1"></i>
                                已禁用渠道（{disabledCount}
                                ）已固定在底部分区
                              </span>
                            </td>
                          </tr>
                        ) : null}
                        <tr
                          className={rowBaseClassName || undefined}
                          data-rlm-channel-row="main"
                          data-rlm-channel-id={ch.id}
                          data-rlm-channel-disabled={channelDisabled ? '1' : '0'}
                          onClick={(e) => {
                            const target = e.target as HTMLElement;
                            if (target.closest('button, a, input, textarea, select, label')) return;
                            toggleChannelPanel(ch.id);
                          }}
                        >
                          {renderMainRowCells()}
                        </tr>
                        {panelOpen ? (
                          <tr
                            className={`${channelDisabled ? 'table-secondary opacity-75' : 'bg-light-subtle'} rlm-channel-detail-row`}
                          >
                            <td colSpan={channelTableCols} className="px-4 py-3">
                              <div className="d-flex flex-wrap align-items-center gap-2 mb-3">
                                <Button
                                  variant={detailPanel === 'stats' ? 'solid' : 'soft'}
                                  tone={detailPanel === 'stats' ? 'primary' : 'default'}
                                  size="sm"
                                  onClick={() =>
                                    setDetailPanelByChannel((prev) => ({
                                      ...prev,
                                      [ch.id]: 'stats',
                                    }))
                                  }
                                >
                                  详细统计
                                </Button>
                              </div>

                              <>
                                <div className="d-flex flex-wrap align-items-center gap-3 small text-muted">
                                  <div className="d-flex align-items-center">
                                    <span className="me-1">消耗:</span>
                                    <span className="font-monospace fw-bold text-dark">{usage?.usd ?? '0'}</span>
                                  </div>
                                  <div className="d-flex align-items-center">
                                    <span className="me-1">首字:</span>
                                    <span className="fw-medium text-dark">
                                      {formatSecondsFromMilliseconds(usage?.avg_first_token_latency)}
                                    </span>
                                  </div>
                                </div>
                                <div className="border rounded-3 p-3 bg-white mt-3">
                                  <div className="d-flex flex-wrap align-items-center gap-3 mb-2">
                                    <div className="d-flex align-items-center gap-2 flex-grow-1">
                                      <div className="d-flex flex-wrap gap-1">
                                        {fieldOptions.map((option) => (
                                          <Button
                                            key={option.value}
                                            variant={detailField === option.value ? 'solid' : 'outline'}
                                            tone={detailField === option.value ? 'primary' : 'default'}
                                            size="sm"
                                            onClick={() => setDetailField(option.value)}
                                          >
                                            {option.label}
                                          </Button>
                                        ))}
                                      </div>
                                    </div>
                                    <div className="d-flex align-items-center gap-2 ms-auto">
                                      <div className="d-flex gap-1">
                                        {granularityOptions.map((option) => (
                                          <Button
                                            key={option.value}
                                            variant={detailGranularity === option.value ? 'solid' : 'outline'}
                                            tone={detailGranularity === option.value ? 'primary' : 'default'}
                                            size="sm"
                                            onClick={() => setDetailGranularity(option.value)}
                                          >
                                            {option.label}
                                          </Button>
                                        ))}
                                      </div>
                                    </div>
                                  </div>
                                  <div className="small text-muted mb-2">
                                    时间区间：{detailSeriesStart || '-'} ~ {detailSeriesEnd || '-'}
                                  </div>
                                  {detailSeriesErr ? (
                                    <div className="mb-2">
                                      <Alert tone="danger" compact>
                                        {detailSeriesErr}
                                      </Alert>
                                    </div>
                                  ) : null}
                                  {detailSeriesLoading ? (
                                    <div className="text-muted small py-4">时间序列加载中…</div>
                                  ) : (
                                    <LineChart
                                      title={`${ch.name || `渠道 #${ch.id}`} · 时间序列`}
                                      labels={detailSeries.map((point) => point.bucket)}
                                      series={[
                                        {
                                          label: fieldMeta[detailField].label,
                                          tone: fieldMeta[detailField].tone,
                                          values: detailSeries.map(fieldMeta[detailField].read),
                                        },
                                      ]}
                                      maxTicks={detailGranularity === 'hour' ? 10 : 14}
                                    />
                                  )}
                                </div>
                              </>
                            </td>
                          </tr>
                        ) : null}
                      </Fragment>
                    );
                  })}
                </>
              )}
            </Table>
          </div>
        </div>
      </SegmentedFrame>

      {/* The dialog exists only while a target is selected, so the old
          "未选择渠道" placeholder branch is gone with it. */}
      <Modal
        open={!!pointerTarget}
        onClose={() => {
          setPointerTarget(null);
          setPointerGroupID('');
        }}
        title={pointerTarget ? `设为指针：${pointerTarget.name || `#${pointerTarget.id}`}` : '设为指针'}
      >
        {pointerGroupOptions.length === 0 ? (
          <div className="text-muted">该渠道未加入任何启用的渠道组，无法设为指针。</div>
        ) : (
          <form
            className="row g-3"
            onSubmit={async (e) => {
              e.preventDefault();
              if (!pointerTarget) return;
              const groupID = Number.parseInt(pointerGroupID, 10) || 0;
              if (groupID <= 0) {
                return;
              }
              const g = pointerGroupOptions.find((x) => x.id === groupID) || null;
              if (
                !window.confirm(
                  `确认将渠道 ${pointerTarget.name || pointerTarget.id} 设为渠道组 ${g?.name || groupID} 的指针？`
                )
              )
                return;
              try {
                await setChannelGroupPointer.mutateAsync({
                  groupID,
                  channelID: pointerTarget.id,
                  pinned: true,
                });
                setPointerTarget(null);
                setPointerGroupID('');
              } catch {
                // Deliberately silent: a failed repoint leaves the modal open
                // on the unchanged selection.
              }
            }}
          >
            <div className="col-12">
              <label className="form-label">选择渠道组</label>
              <Select value={pointerGroupID} onChange={(e) => setPointerGroupID(e.target.value)}>
                {pointerGroupOptions.map((g) => (
                  <option key={g.id} value={String(g.id)}>
                    {g.name} #{g.id}
                  </option>
                ))}
              </Select>
              <div className="form-text small text-muted">指针会固定到该渠道，直到被重新设置。</div>
            </div>

            <div className="modal-footer border-top-0 px-0 pb-0">
              <Button
                variant="quiet"
                onClick={() => {
                  setPointerTarget(null);
                  setPointerGroupID('');
                }}
              >
                取消
              </Button>
              <Button type="submit" variant="solid" tone="primary" wide>
                确认设置
              </Button>
            </div>
          </form>
        )}
      </Modal>

      <Modal
        open={creating}
        onClose={() => setCreating(false)}
        title="新建渠道"
        size="lg"
        scrollable
        bodyTone="muted"
        footer={
          <Button variant="quiet" onClick={() => setCreating(false)}>
            取消
          </Button>
        }
      >
        <ChannelCommonTab mode="create" channelGroups={channelGroups} onDone={() => setCreating(false)} />
      </Modal>

      <Modal
        open={settingsChannelID !== null}
        onClose={() => setSettingsChannelID(null)}
        title={settingsChannelID ? `渠道设置：${settingsChannel?.name || `#${settingsChannelID}`}` : '渠道设置'}
        size="lg"
        scrollable
        bodyTone="muted"
        footer={
          <Button variant="quiet" onClick={() => setSettingsChannelID(null)}>
            关闭
          </Button>
        }
      >
        {loading ? (
          <div className="text-muted">加载中…</div>
        ) : !settingsChannel ? (
          <div className="text-muted">加载失败。</div>
        ) : (
          <>
            <div className="d-flex flex-wrap align-items-center gap-2 mb-3">
              <span className="fw-semibold text-dark">{settingsChannel.name || `渠道 #${settingsChannel.id}`}</span>
              <span className="text-muted small">#{settingsChannel.id}</span>
              <span className="text-muted small">({settingsChannel.type})</span>
            </div>

            {/* Keyed by channel: switching targets remounts the editor with a
                fresh draft, which is what the old reset effect enforced. */}
            <ChannelCommonTab key={settingsChannel.id} channel={settingsChannel} channelGroups={channelGroups} />
          </>
        )}
      </Modal>
    </div>
  );
}
