import { useRef, useState } from 'react';

import { useAuth } from '../auth/AuthContext';
import { DateRangePicker, SelectPicker } from '../components/DateRangePicker';
import { SegmentedFrame } from '../components/SegmentedFrame';
import {
  UsageAdvancedFiltersDropdown,
  type UsageAdvancedFiltersDropdownHandle,
} from '../components/UsageAdvancedFiltersDropdown';
import { useUsageEventDetail, useUsageEvents, useUsageWindow } from '../data/usage';
import { useTokensByID } from '../data/tokens';
import type { UsageEventDetail } from '../api/usage';
import { UsageEventsCard } from './usage/UsageEventsCard';
import { UsageSummaryCard } from './usage/UsageSummaryCard';
import { formatLocalDate, formatLocalDateTimeMinute } from './usage/usageUtils';
import { todayDateInputLocal } from '../utils/dateInput';

type UsageFilters = {
  start: string;
  end: string;
  allTime: boolean;
  limit: number;
  filterKey: string;
  filterModel: string;
};

function defaultFilters(): UsageFilters {
  const today = todayDateInputLocal();
  return { start: today, end: today, allTime: false, limit: 50, filterKey: '', filterModel: '' };
}

export function UsagePage() {
  const { user } = useAuth();

  // `draft` is what the controls display; `applied` is what actually drives the
  // queries below. They start out equal (mirroring the mount-time fetch this page
  // always used to do) and are only re-synced by 更新/重置/翻页 — every other control
  // only edits `draft`, the same "changes stage until you click 更新" behavior the
  // page had before the data layer existed.
  const [draft, setDraft] = useState<UsageFilters>(defaultFilters);
  const [applied, setApplied] = useState<UsageFilters>(defaultFilters);
  const advRef = useRef<UsageAdvancedFiltersDropdownHandle | null>(null);

  const [beforeID, setBeforeID] = useState<number | undefined>(undefined);
  const [beforeStack, setBeforeStack] = useState<number[]>([]);
  const [expandedID, setExpandedID] = useState<number | null>(null);

  const allTimeActive = applied.allTime && !applied.start.trim() && !applied.end.trim();
  const windowQuery = useUsageWindow(applied);
  const eventsQuery = useUsageEvents({ ...applied, beforeID });
  const tokensQuery = useTokensByID();
  const detailQuery = useUsageEventDetail(expandedID);

  const loading = windowQuery.isFetching || eventsQuery.isFetching;
  const err = windowQuery.error?.message || eventsQuery.error?.message || '';
  const events = eventsQuery.data?.events ?? [];
  const tokenByID = tokensQuery.data ?? {};

  const canPrev = beforeStack.length > 0;
  const canNext = !!eventsQuery.data?.next_before_id && events.length === applied.limit;

  // Card still keys its detail lookup by event id; only `expandedID` is ever
  // populated since only one row is fetched/expanded at a time.
  const detailByEventID: Record<number, UsageEventDetail> =
    expandedID !== null && detailQuery.data ? { [expandedID]: detailQuery.data } : {};
  const detailLoadingID = expandedID !== null && detailQuery.isFetching ? expandedID : null;
  // The fetch itself is automatic (see useUsageEventDetail above, keyed on
  // expandedID) — this is a no-op kept only because UsageEventsCard's onClick
  // calls it alongside setExpandedID.
  const loadDetail = () => {};

  // When the range is cleared by hand, the server picks a window and echoes it
  // back. The picker displays that echo instead of it being written into
  // `draft`/`applied`: writing it back would make the response an input to the
  // query that produced it, refiring both queries under a new key.
  const echoDay = !allTimeActive && windowQuery.data ? formatLocalDate(String(windowQuery.data.since)) : '';
  const draftStartText = draft.start || echoDay;
  const draftEndText = draft.end || draft.start || echoDay;

  const rangeSinceText = windowQuery.data ? formatLocalDateTimeMinute(String(windowQuery.data.since)) : '';
  const rangeUntilText = windowQuery.data ? formatLocalDateTimeMinute(String(windowQuery.data.until)) : '';

  const selfEmail = (user?.email || user?.username || '').toString().trim() || '-';
  const selfID = typeof user?.id === 'number' ? user.id : '-';

  function resetCursor() {
    setBeforeID(undefined);
    setBeforeStack([]);
    setExpandedID(null);
  }

  const onPrevPage = () => {
    const nextStack = beforeStack.slice(0, -1);
    setBeforeStack(nextStack);
    setExpandedID(null);
    setBeforeID(nextStack.length > 0 ? nextStack[nextStack.length - 1] : undefined);
  };

  const onNextPage = () => {
    const next = eventsQuery.data?.next_before_id;
    if (!next) return;
    setBeforeStack((s) => [...s, next]);
    setExpandedID(null);
    setBeforeID(next);
  };

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div>
          <div className="d-flex justify-content-between align-items-center mb-3">
            <div>
              <h3 className="mb-1 fw-bold">用量统计</h3>
              <div className="text-muted small">按日期范围汇总用量，并支持事件明细查看。</div>
            </div>
          </div>

          {err ? (
            <div className="alert alert-danger mb-3">
              <span className="me-2 material-symbols-rounded">warning</span>
              {err}
            </div>
          ) : null}

          <div className="card border-0 shadow-sm mb-0">
            <div className="card-body py-3 px-4">
              <div className="d-flex flex-wrap align-items-end gap-3">
                <div className="d-flex flex-wrap align-items-center gap-2">
                  <div className="text-muted smaller fw-medium text-nowrap">时间区间</div>
                  <DateRangePicker
                    start={draftStartText}
                    end={draftEndText}
                    onChange={(r) => {
                      const isAll = !r.start.trim() && !r.end.trim();
                      setDraft((d) => ({ ...d, start: r.start, end: r.end, allTime: isAll }));
                      setBeforeStack([]);
                      setExpandedID(null);
                    }}
                    loading={loading}
                  />
                </div>

                <div className="d-flex flex-wrap align-items-center gap-2">
                  <div className="text-muted smaller fw-medium text-nowrap">显示条数</div>
                  <SelectPicker
                    value={draft.limit}
                    options={[
                      { label: '20', value: 20 },
                      { label: '50', value: 50 },
                      { label: '100', value: 100 },
                    ]}
                    label="条"
                    onChange={(val) => {
                      setDraft((d) => ({ ...d, limit: val }));
                      setBeforeStack([]);
                      setExpandedID(null);
                    }}
                  />
                </div>

                <div className="d-flex align-items-center gap-2">
                  <UsageAdvancedFiltersDropdown
                    ref={advRef}
                    disabled={loading}
                    toggleTestId="usage-adv-toggle"
                    fields={[
                      {
                        inputId: 'usageFilterKeyValue',
                        label: 'Key',
                        title: 'Key 名称',
                        placeholder: '输入 Key 名称',
                        value: draft.filterKey,
                        onChange: (v) => {
                          setDraft((d) => ({ ...d, filterKey: v }));
                          setBeforeStack([]);
                          setExpandedID(null);
                        },
                      },
                      {
                        inputId: 'usageFilterModelValue',
                        label: '模型',
                        title: '模型',
                        placeholder: '输入模型名',
                        value: draft.filterModel,
                        onChange: (v) => {
                          setDraft((d) => ({ ...d, filterModel: v }));
                          setBeforeStack([]);
                          setExpandedID(null);
                        },
                      },
                    ]}
                  />
                </div>

                <div className="ms-auto d-flex gap-2">
                  <button
                    className="btn btn-primary btn-sm"
                    type="button"
                    disabled={loading}
                    onClick={() => {
                      setApplied(draft);
                      resetCursor();
                    }}
                  >
                    <span className="material-symbols-rounded me-1">refresh</span>
                    更新
                  </button>
                  <button
                    className="btn btn-light border btn-sm"
                    type="button"
                    disabled={loading}
                    onClick={() => {
                      const fresh = defaultFilters();
                      setDraft(fresh);
                      setApplied(fresh);
                      advRef.current?.close();
                      resetCursor();
                    }}
                  >
                    重置
                  </button>
                </div>
              </div>
            </div>
          </div>
        </div>

        {loading ? (
          <div className="text-muted">加载中…</div>
        ) : windowQuery.data ? (
          <div className="row g-4">
            <div className="col-12">
              <UsageSummaryCard
                data={windowQuery.data}
                rangeSinceText={rangeSinceText}
                rangeUntilText={rangeUntilText}
              />
            </div>

            <div className="col-12">
              <UsageEventsCard
                events={events}
                tokenByID={tokenByID}
                expandedID={expandedID}
                setExpandedID={setExpandedID}
                loadDetail={loadDetail}
                detailLoadingID={detailLoadingID}
                detailByEventID={detailByEventID}
                canPrev={canPrev}
                canNext={canNext}
                loading={loading}
                onPrevPage={onPrevPage}
                onNextPage={onNextPage}
                selfEmail={selfEmail}
                selfID={selfID}
              />
            </div>
          </div>
        ) : null}
      </SegmentedFrame>
    </div>
  );
}
