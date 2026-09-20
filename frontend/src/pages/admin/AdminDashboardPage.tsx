import { useState } from 'react';
import { Link } from 'react-router-dom';

import { useAdminDashboard, useAdminUsageSeries } from '../../data/adminDashboard';
import { useAuth } from '../../auth/AuthContext';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { formatIntComma } from '../../format/int';
import { Alert } from '../../ui/Alert';
import { Badge } from '../../ui/Badge';
import { Button } from '../../ui/Button';
import { Card } from '../../ui/Card';
import { UsageAdminTimeSeriesCard } from './usage/UsageAdminTimeSeriesCard';
import {
  type UsageAdminDetailField,
  type UsageAdminDetailGranularity,
  usageAdminFieldOptions,
  usageAdminGranularityOptions,
} from './usage/usageAdminUtils';

export function AdminDashboardPage() {
  const { user } = useAuth();
  const [detailField, setDetailField] = useState<UsageAdminDetailField>('requests');
  const [detailGranularity, setDetailGranularity] = useState<UsageAdminDetailGranularity>('hour');

  const { data, error } = useAdminDashboard();
  const series = useAdminUsageSeries(detailGranularity);

  if (error) {
    return (
      <div className="mb-4">
        <Alert tone="danger" icon={<i className="ri-alert-line"></i>}>
          {error.message}
        </Alert>
      </div>
    );
  }

  if (!data) {
    return (
      <Card>
        <div className="text-muted small d-flex align-items-center">
          <span className="spinner-border spinner-border-sm me-2" role="status" aria-hidden="true"></span>
          正在加载…
        </div>
      </Card>
    );
  }

  const tz = data.admin_time_zone || 'Asia/Shanghai';
  const stats = data.stats;

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="d-flex align-items-center justify-content-between">
          <h2 className="h4 fw-bold mb-0 text-dark">仪表盘</h2>
          <Badge>{tz} 时间</Badge>
        </div>

        <div className="row g-4 mb-0">
          <MetricCard icon="ri-group-line" label="总用户数" value={formatIntComma(stats.users_count)} tone="primary" />
          <MetricCard
            icon="ri-git-merge-line"
            label="上游渠道"
            value={formatIntComma(stats.channels_count)}
            tone="success"
          />
          <MetricCard
            icon="ri-server-line"
            label="上游节点"
            value={formatIntComma(stats.endpoints_count)}
            tone="info"
          />
        </div>

        <Card
          padding="roomy"
          header={
            <div className="d-flex justify-content-between align-items-center">
              <div className="d-flex align-items-center">
                <div className="rounded-circle bg-primary p-1 me-2"></div>
                <span className="fw-bold text-dark text-uppercase small">今日概览</span>
                <span className="text-muted smaller ms-2">{tz} 时间</span>
              </div>
              <div className="text-muted smaller">
                <i className="ri-time-line me-1 text-primary"></i> 当前快照
              </div>
            </div>
          }
        >
          <div className="row text-center">
            <div className="col-md-4 border-end">
              <h6 className="text-muted mb-2 small fw-semibold text-uppercase">总请求数</h6>
              <h2 className="fw-bold text-dark">{formatIntComma(stats.requests_today)}</h2>
            </div>
            <div className="col-md-4 border-end">
              <h6 className="text-muted mb-2 small fw-semibold text-uppercase">Token 消耗</h6>
              <h2 className="fw-bold text-dark">{formatIntComma(stats.tokens_today)}</h2>
              <div className="small text-muted font-monospace mt-1">
                <span className="me-2">
                  <i className="ri-arrow-up-line text-success"></i> {formatIntComma(stats.input_tokens_today)}
                </span>
                <span>
                  <i className="ri-arrow-down-line text-primary"></i> {formatIntComma(stats.output_tokens_today)}
                </span>
              </div>
            </div>
            <div className="col-md-4">
              <h6 className="text-muted mb-2 small fw-semibold text-uppercase">预估消费</h6>
              <h2 className="fw-bold text-primary">{stats.cost_today}</h2>
            </div>
          </div>
        </Card>

        <UsageAdminTimeSeriesCard
          detailSeries={series.points}
          detailSeriesStart={series.start}
          detailSeriesEnd={series.end}
          detailSeriesErr={series.error ? series.error.message : ''}
          detailSeriesLoading={series.isPending}
          detailField={detailField}
          detailGranularity={detailGranularity}
          fieldOptions={usageAdminFieldOptions}
          granularityOptions={usageAdminGranularityOptions}
          onFieldChange={setDetailField}
          onGranularityChange={setDetailGranularity}
        />

        <div className="row g-4 mb-0">
          <div className="col-md-6">
            <Card fullHeight>
              <h5 className="card-title fw-bold mb-3 text-dark h6">快捷操作</h5>
              <div className="d-grid gap-2">
                <Button component={Link} to="/admin/channels" variant="outline" tone="primary">
                  <i className="ri-git-merge-line me-2 text-primary"></i> 管理上游渠道
                </Button>
                <Button component={Link} to="/admin/users" variant="outline" tone="primary">
                  <i className="ri-user-settings-line me-2 text-primary"></i> 管理用户与权限
                </Button>
              </div>
            </Card>
          </div>

          <div className="col-md-6">
            <Card fullHeight>
              <h5 className="card-title fw-bold mb-3 text-dark h6">系统信息</h5>
              <ul className="list-unstyled mb-0">
                <li className="mb-3 d-flex align-items-center">
                  <span className="text-muted small me-2">当前用户:</span>
                  <strong className="text-dark">{user?.email || '-'}</strong>
                </li>
                <li className="mb-3 d-flex align-items-center">
                  <span className="text-muted small me-2">角色权限:</span>
                  <Badge tone="primary" pill>
                    {user?.role || '-'}
                  </Badge>
                </li>
              </ul>
            </Card>
          </div>
        </div>
      </SegmentedFrame>
    </div>
  );
}

function MetricCard({ icon, label, value, tone }: { icon: string; label: string; value: string; tone: string }) {
  return (
    <div className="col-md-4">
      <Card fullHeight>
        <div className="d-flex align-items-center">
          <div className={`bg-${tone} bg-opacity-10 p-3 rounded-circle me-3`}>
            <i className={`${icon} fs-4 text-${tone}`}></i>
          </div>
          <div>
            <h6 className="text-muted text-uppercase mb-1 small fw-semibold">{label}</h6>
            <h3 className="mb-0 fw-bold text-dark">{value}</h3>
          </div>
        </div>
      </Card>
    </div>
  );
}
