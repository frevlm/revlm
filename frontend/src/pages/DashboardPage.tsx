import { useState } from 'react';
import { Link } from 'react-router-dom';

import { useDashboard, useDashboardSeries } from '../data/dashboard';
import type { UsageGranularity } from '../data/usage';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { Alert } from '../ui/Alert';
import { Badge } from '../ui/Badge';
import { Button } from '../ui/Button';
import { Card } from '../ui/Card';
import { formatIntComma } from '../format/int';
import { UsageTimeSeriesCard } from './usage/UsageTimeSeriesCard';

type DetailField = 'usd' | 'requests' | 'avg_first_token_latency';

const fieldOptions: Array<{ value: DetailField; label: string }> = [
  { value: 'requests', label: '请求数' },
  { value: 'usd', label: '消耗 (USD)' },
  { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
];

const granularityOptions: Array<{ value: UsageGranularity; label: string }> = [
  { value: 'hour', label: '按小时' },
  { value: 'day', label: '按天' },
];

export function DashboardPage() {
  const [detailField, setDetailField] = useState<DetailField>('requests');
  const [detailGranularity, setDetailGranularity] = useState<UsageGranularity>('hour');

  const { data, error } = useDashboard();
  const series = useDashboardSeries(detailGranularity);

  const todayUsageUSD = data?.today_usage_usd || '-';
  const todayRequests = data ? formatIntComma(data.today_requests) : '-';
  const todayRPM = data ? formatIntComma(data.today_rpm) : '-';

  return (
    <div className="fade-in-up">
      {error ? (
        <Alert tone="danger" icon={<span className="material-symbols-rounded">warning</span>}>
          {error.message}
        </Alert>
      ) : null}

      <SegmentedFrame>
        <div className="row g-4">
          <div className="col-12">
            <div className="row g-4">
              <div className="col-md-6 col-xl-3">
                <Card fullHeight>
                  <div className="d-flex align-items-center mb-3">
                    <div className="bg-primary bg-opacity-10 text-primary rounded-pill p-2 me-3">
                      <span className="fs-4 px-1 material-symbols-rounded">attach_money</span>
                    </div>
                    <h6 className="card-title mb-0 fw-bold">今日费用</h6>
                  </div>
                  <div className="mb-0">
                    <h3 className="fw-bold mb-1">{todayUsageUSD}</h3>
                    <p className="text-muted small mb-0">预估消耗 (USD)</p>
                  </div>
                </Card>
              </div>

              <div className="col-md-6 col-xl-3">
                <Card fullHeight>
                  <div className="d-flex align-items-center mb-3">
                    <div className="bg-info bg-opacity-10 text-info rounded-pill p-2 me-3">
                      <span className="fs-4 px-1 material-symbols-rounded">chat</span>
                    </div>
                    <h6 className="card-title mb-0 fw-bold">今日请求</h6>
                  </div>
                  <div className="mb-0">
                    <h3 className="fw-bold mb-1">{todayRequests}</h3>
                    <div className="text-muted small">
                      <Badge>RPM: {todayRPM}</Badge>
                      <span className="ms-1">次/分钟</span>
                    </div>
                  </div>
                </Card>
              </div>

              <div className="col-md-6 col-xl-3">
                <Card fullHeight dashed padding="roomy" align="center">
                  <div className="bg-light text-muted rounded-circle p-2 mb-2">
                    <span className="fs-4 material-symbols-rounded">account_balance_wallet</span>
                  </div>
                  <h6 className="fw-bold small mb-1">按量计费</h6>
                  <p className="text-muted small mb-2">模型调用直接从余额扣费。</p>
                  <Button component={Link} to="/topup" variant="outline" tone="primary" size="sm">
                    余额充值
                  </Button>
                </Card>
              </div>
            </div>
          </div>
        </div>

        <UsageTimeSeriesCard
          rangeSinceText={series.start || '-'}
          rangeUntilText={series.end || '-'}
          detailSeries={series.points}
          detailSeriesErr={series.error ? series.error.message : ''}
          detailSeriesLoading={series.isPending}
          detailField={detailField}
          setDetailField={setDetailField}
          detailGranularity={detailGranularity}
          setDetailGranularity={setDetailGranularity}
          fieldOptions={fieldOptions}
          granularityOptions={granularityOptions}
        />
      </SegmentedFrame>
    </div>
  );
}
