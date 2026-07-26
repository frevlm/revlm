import { Fragment } from 'react';

import type { UserToken } from '../../api/tokens';
import type { UsageEvent, UsageEventDetail } from '../../api/usage';
import { formatLatencyPairSeconds } from '../../format/duration';
import { formatIntComma } from '../../format/int';
import {
  costLabel,
  serviceTierBadgeLabel,
  serviceTierText,
  errorText,
  formatDecimalPlain,
  formatLocalDateTime,
  formatUSD,
  tokenNameFromMap,
  tokensPerSecond,
} from './usageUtils';

function cachedTokens(e: UsageEvent): number {
  const fromAggregate = typeof e.cache_creation_tokens === 'number' ? e.cache_creation_tokens : 0;
  const fromParts =
    (typeof e.cache_creation_5m_tokens === 'number' ? e.cache_creation_5m_tokens : 0) +
    (typeof e.cache_creation_1h_tokens === 'number' ? e.cache_creation_1h_tokens : 0);
  const creation = fromAggregate > 0 ? fromAggregate : fromParts;
  const read = typeof e.cache_read_tokens === 'number' && e.cache_read_tokens > 0 ? e.cache_read_tokens : 0;
  return read + creation;
}

export function UsageEventsCard({
  events,
  tokenByID,
  expandedID,
  setExpandedID,
  loadDetail,
  detailLoadingID,
  detailByEventID,
  canPrev,
  canNext,
  loading,
  onPrevPage,
  onNextPage,
}: {
  events: UsageEvent[];
  tokenByID: Record<number, UserToken>;
  expandedID: number | null;
  setExpandedID: (value: number | null) => void;
  loadDetail: (eventID: number) => void | Promise<void>;
  detailLoadingID: number | null;
  detailByEventID: Record<number, UsageEventDetail>;
  canPrev: boolean;
  canNext: boolean;
  loading: boolean;
  onPrevPage: () => void;
  onNextPage: () => void;
}) {
  return (
    <div className="card mb-0 overflow-hidden">
      <div className="card-header d-flex justify-content-between align-items-center">
        <span>请求明细</span>
        <div className="d-flex gap-2">
          <button
            type="button"
            className="btn btn-sm btn-outline-secondary"
            disabled={!canPrev || loading}
            onClick={onPrevPage}
          >
            上一页
          </button>
          <button
            type="button"
            className="btn btn-sm btn-outline-secondary"
            disabled={!canNext || loading}
            onClick={onNextPage}
          >
            下一页
          </button>
        </div>
      </div>
      <div className="table-responsive rlm-table-responsive-no-x border-0 rounded-0">
        <table className="table table-hover align-middle mb-0 rlm-table-fit">
          <colgroup>
            <col style={{ width: '13%' }} />
            <col />
            <col className="rlm-usage-col-status" />
            <col className="rlm-usage-col-latency" />
            <col className="rlm-usage-col-tokens" />
            <col className="rlm-usage-col-cost" />
            <col className="rlm-usage-col-key" />
            <col className="rlm-usage-col-request" />
          </colgroup>
          <thead>
            <tr>
              <th>时间</th>
              <th>模型 / 接口</th>
              <th className="rlm-usage-cell-compact">状态</th>
              <th className="text-end rlm-usage-cell-compact">耗时 / 首字</th>
              <th className="text-end rlm-usage-cell-compact">Tokens</th>
              <th className="text-end rlm-usage-cell-compact">费用</th>
              <th className="rlm-usage-cell-compact">Key</th>
              <th>Request ID</th>
            </tr>
          </thead>
          <tbody className="small">
            {events.map((e) => {
              const endpoint = (e.endpoint || '').trim() || '-';
              const model = (e.model || e.model_name || '').trim() || '-';
              const keyName = tokenNameFromMap(tokenByID, e.token_id);
              const code = e.status_code ? String(e.status_code) : '-';
              const ok = code === '200';
              const cached = cachedTokens(e);
              const cost = costLabel(e);
              const errText = errorText(e.error_class, e.error_message);
              const detail = detailByEventID[e.id];
              const pricingBreakdown = detail?.pricing_breakdown;
              const tierBadge = serviceTierBadgeLabel(pricingBreakdown?.service_tier ?? e.service_tier);
              const markers = [e.is_stream ? '流式' : '', tierBadge].filter(Boolean).join(' · ');

              return (
                <Fragment key={e.id}>
                  <tr
                    className="rlm-usage-row"
                    role="button"
                    onClick={() => {
                      const next = expandedID === e.id ? null : e.id;
                      setExpandedID(next);
                      if (next) void loadDetail(e.id);
                    }}
                  >
                    <td className="text-nowrap font-monospace">
                      <i
                        className={`ri-arrow-right-s-line rlm-usage-chevron text-muted align-middle ${expandedID === e.id ? 'rotate-90' : ''}`}
                      ></i>
                      <span className="align-middle">{formatLocalDateTime(String(e.time))}</span>
                    </td>
                    <td>
                      <div className="font-monospace">{model}</div>
                      <div className="text-muted smaller">
                        {endpoint}
                        {markers ? ` · ${markers}` : ''}
                      </div>
                    </td>
                    <td className="rlm-usage-cell-compact" title={errText || undefined}>
                      <span className={`rlm-dot-state${ok ? '' : ' danger'}`}>{code}</span>
                    </td>
                    <td className="text-end font-monospace text-muted rlm-usage-cell-compact">
                      {formatLatencyPairSeconds(e.latency_ms, e.first_token_latency_ms)}
                    </td>
                    <td className="text-end font-monospace rlm-usage-cell-compact">
                      <div>
                        {formatIntComma(e.input_tokens)} <span className="text-muted">→</span>{' '}
                        {formatIntComma(e.output_tokens)}
                      </div>
                      {cached > 0 ? <div className="text-muted smaller">缓存 {formatIntComma(cached)}</div> : null}
                    </td>
                    <td className="text-end font-monospace rlm-usage-cell-compact">{cost}</td>
                    <td className="text-muted rlm-usage-cell-compact">{keyName && keyName !== '-' ? keyName : '-'}</td>
                    <td
                      className="font-monospace text-muted user-select-all"
                      style={{ maxWidth: 160, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                      title={e.request_id}
                    >
                      {e.request_id}
                    </td>
                  </tr>
                  {expandedID === e.id ? (
                    <tr className="rlm-usage-detail-row">
                      <td colSpan={8} className="p-0 border-0">
                        <div className="rlm-usage-detail-panel">
                          {detailLoadingID === e.id ? <div className="text-muted small">加载详情中…</div> : null}
                          {detail ? (
                            <div className="row g-3 small">
                              <div className="col-12 col-lg-4">
                                <div className="text-muted smaller">Event ID</div>
                                <div className="font-monospace">{e.id}</div>
                              </div>
                              <div className="col-12 col-lg-4">
                                <div className="text-muted smaller">Request ID</div>
                                <div className="font-monospace user-select-all">{e.request_id || '-'}</div>
                              </div>
                              <div className="col-12 col-lg-4">
                                <div className="text-muted smaller">Response ID</div>
                                <div className="font-monospace user-select-all">{e.response_id || '-'}</div>
                              </div>
                              <div className="col-12 col-lg-4">
                                <div className="text-muted smaller">Service Tier</div>
                                <div className="font-monospace">
                                  {serviceTierText(pricingBreakdown?.service_tier || e.service_tier)}
                                </div>
                              </div>
                              <div className="col-12 col-lg-4">
                                <div className="text-muted smaller">Tokens/s</div>
                                <div className="font-monospace">{formatIntComma(tokensPerSecond(e))}</div>
                              </div>
                              <div className="col-12 col-lg-4">
                                <div className="text-muted smaller">错误</div>
                                <div className="font-monospace">{errText || '-'}</div>
                              </div>

                              {pricingBreakdown ? (
                                <div className="col-12">
                                  <div className="text-muted smaller">费用明细</div>
                                  <div className="font-monospace">
                                    <div>
                                      计费输入 {formatIntComma(pricingBreakdown.input_tokens_billable || 0)} · 输出{' '}
                                      {formatIntComma(pricingBreakdown.output_tokens_total || 0)}
                                      {(pricingBreakdown.input_tokens_cache_read || 0) > 0
                                        ? ` · 缓存读取 ${formatIntComma(pricingBreakdown.input_tokens_cache_read)}`
                                        : ''}
                                      {(pricingBreakdown.input_tokens_cache_creation_1h || 0) > 0
                                        ? ` · 缓存创建·5m ${formatIntComma(pricingBreakdown.input_tokens_cache_creation_5m || 0)} · 缓存创建·1h ${formatIntComma(pricingBreakdown.input_tokens_cache_creation_1h)}`
                                        : (pricingBreakdown.input_tokens_cache_creation || 0) > 0
                                          ? ` · 缓存创建 ${formatIntComma(pricingBreakdown.input_tokens_cache_creation)}`
                                          : ''}
                                    </div>
                                    <div className="mt-1">
                                      合计 {formatUSD(pricingBreakdown.final_cost_usd || '0')}
                                      <span className="text-muted smaller">
                                        {' '}
                                        （倍率: tier×
                                        {formatDecimalPlain(pricingBreakdown.tier_multiplier ?? 1)} × channel×
                                        {formatDecimalPlain(pricingBreakdown.channel_multiplier ?? 1)}）
                                      </span>
                                    </div>
                                  </div>
                                </div>
                              ) : null}
                            </div>
                          ) : (
                            <div className="text-muted small">（展开后自动加载费用明细）</div>
                          )}
                        </div>
                      </td>
                    </tr>
                  ) : null}
                </Fragment>
              );
            })}
            {events.length === 0 ? (
              <tr>
                <td colSpan={8} className="text-center py-5 text-muted small">
                  暂无请求记录
                </td>
              </tr>
            ) : null}
          </tbody>
        </table>
      </div>
    </div>
  );
}
