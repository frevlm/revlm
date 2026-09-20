import { useRef, useState } from 'react';

import { useAuth } from '../../auth/AuthContext';
import type { UsageEventDetail } from '../../api/admin/usage';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import type { UsageAdvancedFiltersDropdownHandle } from '../../components/UsageAdvancedFiltersDropdown';
import {
  useAdminUsageEventDetail,
  useAdminUsageEvents,
  useAdminUsageSummary,
  type AdminUsageCursor,
  type AdminUsageFilters,
} from '../../data/usageAdmin';
import { UsageAdminEventsCard } from './usage/UsageAdminEventsCard';
import { UsageAdminFilterBar } from './usage/UsageAdminFilterBar';
import { UsageAdminSummaryCard } from './usage/UsageAdminSummaryCard';
import { UsageAdminTopUsersCard } from './usage/UsageAdminTopUsersCard';

function defaultFilters(): AdminUsageFilters {
  return {
    start: '',
    end: '',
    allTime: false,
    limit: 50,
    filterUser: '',
    filterUserID: undefined,
    filterChannel: '',
    filterChannelID: undefined,
    filterModel: '',
    filterModelExact: undefined,
  };
}

export function UsageAdminPage() {
  useAuth();

  // `draft` is what the filter bar displays; `applied` drives the summary query
  // below and only advances on 更新/重置 — the same "changes stage until you click
  // 更新" behavior this page had before the data layer existed. Paging leaves both
  // alone and only moves `cursor`.
  const [draft, setDraft] = useState<AdminUsageFilters>(defaultFilters);
  const [applied, setApplied] = useState<AdminUsageFilters>(defaultFilters);
  const [cursor, setCursor] = useState<AdminUsageCursor>({});
  const advRef = useRef<UsageAdvancedFiltersDropdownHandle | null>(null);

  const [expandedID, setExpandedID] = useState<number | null>(null);

  const cursorActive = cursor.beforeID !== undefined || cursor.afterID !== undefined;

  const summary = useAdminUsageSummary(applied);
  const events = useAdminUsageEvents(applied, cursor);
  const detail = useAdminUsageEventDetail(expandedID);

  // Paging only ever refetches events (see useAdminUsageEvents): window and
  // top_users keep showing whatever `summary` last resolved, exactly like the
  // old code's manual `prev?.window ?? nextData.window` merge, but for free —
  // there is no merge because there is only ever one query that owns them.
  const page = cursorActive ? events.data : summary.data;
  const windowStats = summary.data?.window;
  const topUsers = summary.data?.top_users ?? [];
  const eventList = page?.events ?? [];
  const canPrev = !!page?.prev_after_id;
  const canNext = !!page?.next_before_id;
  const loading = summary.isFetching || events.isFetching;
  const err = summary.error?.message || events.error?.message || '';

  // Card still keys its detail lookup by event id; only `expandedID` is ever
  // populated since only one row is fetched/expanded at a time.
  const detailByEventID: Record<number, UsageEventDetail> =
    expandedID !== null && detail.data ? { [expandedID]: detail.data } : {};
  const detailLoadingID = expandedID !== null && detail.isFetching ? expandedID : null;

  function resetCursor() {
    setCursor({});
  }

  function handleDateRangeChange(range: { start: string; end: string }) {
    const isAll = !range.start.trim() && !range.end.trim();
    setDraft((d) => ({ ...d, start: range.start, end: range.end, allTime: isAll }));
    resetCursor();
  }

  function handleUserChange(value: string) {
    setDraft((d) => ({ ...d, filterUser: value, filterUserID: undefined }));
    resetCursor();
  }

  function handleChannelChange(value: string) {
    setDraft((d) => ({ ...d, filterChannel: value, filterChannelID: undefined }));
    resetCursor();
  }

  function handleModelChange(value: string) {
    setDraft((d) => ({ ...d, filterModel: value, filterModelExact: undefined }));
    resetCursor();
  }

  function handleLimitChange(value: number) {
    setDraft((d) => ({ ...d, limit: value }));
    resetCursor();
  }

  function handleRefresh() {
    setApplied(draft);
    resetCursor();
  }

  function handleReset() {
    const fresh = defaultFilters();
    setDraft(fresh);
    setApplied(fresh);
    advRef.current?.close();
    resetCursor();
  }

  function handleToggleEvent(eventID: number) {
    setExpandedID((prev) => (prev === eventID ? null : eventID));
  }

  function handlePrevPage() {
    const nextAfterID = page?.prev_after_id;
    if (!nextAfterID) return;
    setCursor({ afterID: nextAfterID });
  }

  function handleNextPage() {
    const nextBeforeID = page?.next_before_id;
    if (!nextBeforeID) return;
    setCursor({ beforeID: nextBeforeID });
  }

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div>
          <div className="d-flex justify-content-between align-items-center mb-4">
            <div>
              <h3 className="mb-1 fw-bold">全站用量统计</h3>
              <div className="text-muted small">系统级数据汇总，涵盖所有用户及上游通道。</div>
            </div>
          </div>

          {err ? (
            <div className="alert alert-danger mb-3">
              <span className="me-2 material-symbols-rounded">warning</span>
              {err}
            </div>
          ) : null}

          <UsageAdminFilterBar
            advRef={advRef}
            start={draft.start}
            end={draft.end}
            loading={loading}
            limit={draft.limit}
            filterUser={draft.filterUser}
            filterChannel={draft.filterChannel}
            filterModel={draft.filterModel}
            onDateRangeChange={handleDateRangeChange}
            onLimitChange={handleLimitChange}
            onUserChange={handleUserChange}
            onChannelChange={handleChannelChange}
            onModelChange={handleModelChange}
            onRefresh={handleRefresh}
            onReset={handleReset}
          />
        </div>

        {loading ? (
          <div className="text-muted">加载中…</div>
        ) : summary.data && windowStats ? (
          <div className="row g-4">
            <div className="col-12">
              <UsageAdminSummaryCard windowStats={windowStats} />
            </div>

            <div className="col-12">
              <UsageAdminTopUsersCard topUsers={topUsers} />
            </div>

            <div className="col-12">
              <UsageAdminEventsCard
                events={eventList}
                expandedID={expandedID}
                detailByEventID={detailByEventID}
                detailLoadingID={detailLoadingID}
                canPrev={canPrev}
                canNext={canNext}
                loading={loading}
                onToggleEvent={handleToggleEvent}
                onPrevPage={handlePrevPage}
                onNextPage={handleNextPage}
              />
            </div>
          </div>
        ) : null}
      </SegmentedFrame>
    </div>
  );
}
