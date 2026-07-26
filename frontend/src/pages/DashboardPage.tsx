import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { getDashboard, type DashboardData } from '../api/dashboard';
import { getUsageTimeSeries, type UsageTimeSeriesPoint } from '../api/usage';
import { formatIntComma } from '../format/int';
import { fillDailyBuckets } from '../utils/timeSeries';
import { UsageTimeSeriesCard } from './usage/UsageTimeSeriesCard';

type DetailField = 'usd' | 'requests' | 'tokens' | 'cache_ratio' | 'avg_first_token_latency' | 'tokens_per_second';
type DetailGranularity = 'hour' | 'day';

export function DashboardPage() {
  const [data, setData] = useState<DashboardData | null>(null);
  const [err, setErr] = useState('');

  const [detailSeries, setDetailSeries] = useState<UsageTimeSeriesPoint[]>([]);
  const [detailSeriesStart, setDetailSeriesStart] = useState('');
  const [detailSeriesEnd, setDetailSeriesEnd] = useState('');
  const [detailSeriesLoading, setDetailSeriesLoading] = useState(false);
  const [detailSeriesErr, setDetailSeriesErr] = useState('');
  const [detailField, setDetailField] = useState<DetailField>('requests');
  const [detailGranularity, setDetailGranularity] = useState<DetailGranularity>('hour');
  const fieldOptions: Array<{
    value: DetailField;
    label: string;
  }> = [
    { value: 'requests', label: '请求数' },
    { value: 'tokens', label: 'Token' },
    { value: 'usd', label: '消耗 (USD)' },
    { value: 'cache_ratio', label: '缓存率 (%)' },
    { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
    { value: 'tokens_per_second', label: 'Tokens/s' },
  ];
  const granularityOptions: Array<{ value: DetailGranularity; label: string }> = [
    { value: 'hour', label: '按小时' },
    { value: 'day', label: '按天' },
  ];

  useEffect(() => {
    let mounted = true;
    (async () => {
      setErr('');
      try {
        const res = await getDashboard();
        if (!res.success) {
          throw new Error(res.message || '加载失败');
        }
        if (mounted) {
          setData(res.data || null);
        }
      } catch (e) {
        if (mounted) {
          setErr(e instanceof Error ? e.message : '加载失败');
          setData(null);
        }
      }
    })();
    return () => {
      mounted = false;
    };
  }, []);

  useEffect(() => {
    let active = true;
    if (detailGranularity === 'hour') {
      setDetailSeriesErr('');
      setDetailSeriesLoading(!data && !err);
      setDetailSeriesStart(data?.today_since || '');
      setDetailSeriesEnd(data?.today_until || '');
      setDetailSeries(data?.charts.time_series_stats || []);
      return () => {
        active = false;
      };
    }
    void (async () => {
      setDetailSeriesErr('');
      setDetailSeriesLoading(true);
      try {
        const res = await getUsageTimeSeries(undefined, undefined, detailGranularity);
        if (!res.success) throw new Error(res.message || '时间序列加载失败');
        if (!active) return;
        const start = res.data?.start || '';
        const end = res.data?.end || '';
        const points = res.data?.points || [];
        setDetailSeriesStart(start);
        setDetailSeriesEnd(end);
        setDetailSeries(
          detailGranularity === 'day'
            ? fillDailyBuckets(points, start, end, (bucket) => ({
                bucket,
                requests: 0,
                tokens: 0,
                usd: 0,
                cache_ratio: 0,
                avg_first_token_latency: 0,
                tokens_per_second: 0,
              }))
            : points
        );
      } catch (e) {
        if (!active) return;
        setDetailSeries([]);
        setDetailSeriesStart('');
        setDetailSeriesEnd('');
        setDetailSeriesErr(e instanceof Error ? e.message : '时间序列加载失败');
      } finally {
        if (active) setDetailSeriesLoading(false);
      }
    })();
    return () => {
      active = false;
    };
  }, [data, detailGranularity, err]);

  const todayUsageUSD = data?.today_usage_usd || '-';
  const todayRequests = data ? formatIntComma(data.today_requests) : '-';
  const todayRPM = data ? formatIntComma(data.today_rpm) : '-';
  const todayTokens = data ? formatIntComma(data.today_tokens) : '-';
  const todayTPM = data ? formatIntComma(data.today_tpm) : '-';

  return (
    <div className="fade-in-up">
      {err ? (
        <div className="alert alert-danger d-flex align-items-center" role="alert">
          <span className="me-2 material-symbols-rounded">warning</span>
          <div>{err}</div>
        </div>
      ) : null}

      <div className="rlm-page-head">
        <h1>控制台</h1>
        <p className="rlm-page-sub">今日用量概况，随每次请求实时更新。</p>
      </div>

      <div className="rlm-stats mb-3">
        <div className="rlm-stat rlm-stat-green">
          <div className="rlm-stat-label">今日费用</div>
          <div className="rlm-stat-value">{todayUsageUSD}</div>
          <div className="rlm-stat-delta">预估消耗 (USD)</div>
        </div>
        <div className="rlm-stat rlm-stat-blue">
          <div className="rlm-stat-label">今日请求</div>
          <div className="rlm-stat-value">{todayRequests}</div>
          <div className="rlm-stat-delta">RPM {todayRPM} 次/分钟</div>
        </div>
        <div className="rlm-stat rlm-stat-clay">
          <div className="rlm-stat-label">今日 Token</div>
          <div className="rlm-stat-value">{todayTokens}</div>
          <div className="rlm-stat-delta">TPM {todayTPM} Tokens/分钟</div>
        </div>
        <div className="rlm-stat rlm-stat-violet">
          <div className="rlm-stat-label">计费方式</div>
          <div className="rlm-stat-value">按量计费</div>
          <div className="rlm-stat-delta">
            模型调用从余额扣费 · <Link to="/topup">余额充值 →</Link>
          </div>
        </div>
      </div>

      <UsageTimeSeriesCard
        rangeSinceText={detailSeriesStart || '-'}
        rangeUntilText={detailSeriesEnd || '-'}
        detailSeries={detailSeries}
        detailSeriesErr={detailSeriesErr}
        detailSeriesLoading={detailSeriesLoading}
        detailField={detailField}
        setDetailField={setDetailField}
        detailGranularity={detailGranularity}
        setDetailGranularity={setDetailGranularity}
        fieldOptions={fieldOptions}
        granularityOptions={granularityOptions}
      />
    </div>
  );
}
