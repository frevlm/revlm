import { formatIntComma } from '../../format/int';
import { LineChart, type ChartTone } from '../../ui/LineChart';

// The three series the core still aggregates. Anything token-shaped is protocol
// knowledge inside usage_details and is not summed by any endpoint.
export type UsageTimeSeriesChartPoint = {
  bucket: string;
  requests: number;
  usd: number;
  avg_first_token_latency: number;
};

export type UsageTimeSeriesField = 'usd' | 'requests' | 'avg_first_token_latency';

export type UsageTimeSeriesGranularity = 'hour' | 'day';

type Option<T> = { value: T; label: string };

const fieldMeta: Record<
  UsageTimeSeriesField,
  { label: string; tone: ChartTone; read: (point: UsageTimeSeriesChartPoint) => number }
> = {
  usd: { label: '消耗 (USD)', tone: 'primary', read: (point) => point.usd },
  requests: { label: '请求数', tone: 'info', read: (point) => point.requests },
  avg_first_token_latency: {
    label: '首字延迟 (s)',
    tone: 'danger',
    read: (point) => point.avg_first_token_latency / 1000,
  },
};

type Props<TPoint extends UsageTimeSeriesChartPoint> = {
  title: string;
  chartTitle: string;
  rangeSinceText: string;
  rangeUntilText: string;
  detailSeries: TPoint[];
  detailSeriesErr: string;
  detailSeriesLoading: boolean;
  detailField: UsageTimeSeriesField;
  detailGranularity: UsageTimeSeriesGranularity;
  fieldOptions: Array<Option<UsageTimeSeriesField>>;
  granularityOptions: Array<Option<UsageTimeSeriesGranularity>>;
  onFieldChange: (value: UsageTimeSeriesField) => void;
  onGranularityChange: (value: UsageTimeSeriesGranularity) => void;
};

export function UsageTimeSeriesChartCard<TPoint extends UsageTimeSeriesChartPoint>({
  title,
  chartTitle,
  rangeSinceText,
  rangeUntilText,
  detailSeries,
  detailSeriesErr,
  detailSeriesLoading,
  detailField,
  detailGranularity,
  fieldOptions,
  granularityOptions,
  onFieldChange,
  onGranularityChange,
}: Props<TPoint>) {
  const meta = fieldMeta[detailField];

  return (
    <div className="card border-0 p-0 overflow-hidden">
      <div className="card-header bg-white py-3 border-bottom px-4">
        <h5 className="mb-0 fw-bold">
          <i className="ri-line-chart-line me-2"></i>
          {title}
        </h5>
      </div>
      <div className="card-body p-4">
        <div className="d-flex flex-wrap align-items-center gap-3 mb-2">
          <div className="d-flex align-items-center gap-2 flex-grow-1">
            <div className="d-flex flex-wrap gap-1">
              {fieldOptions.map((option) => (
                <button
                  key={option.value}
                  type="button"
                  className={`btn btn-sm ${detailField === option.value ? 'btn-primary' : 'btn-outline-secondary'}`}
                  onClick={() => onFieldChange(option.value)}
                >
                  {option.label}
                </button>
              ))}
            </div>
          </div>
          <div className="d-flex align-items-center gap-2 ms-auto">
            <div className="d-flex gap-1">
              {granularityOptions.map((option) => (
                <button
                  key={option.value}
                  type="button"
                  className={`btn btn-sm ${detailGranularity === option.value ? 'btn-primary' : 'btn-outline-secondary'}`}
                  onClick={() => onGranularityChange(option.value)}
                >
                  {option.label}
                </button>
              ))}
            </div>
          </div>
        </div>

        <div className="small text-muted mb-2">
          时间区间：{rangeSinceText || '-'} ~ {rangeUntilText || '-'}
        </div>
        {detailSeriesErr ? <div className="alert alert-danger py-2 mb-2">{detailSeriesErr}</div> : null}
        {detailSeriesLoading ? (
          <div className="text-muted small py-4">时间序列加载中…</div>
        ) : (
          <LineChart
            title={chartTitle}
            labels={detailSeries.map((point) => point.bucket)}
            series={[{ label: meta.label, tone: meta.tone, values: detailSeries.map(meta.read) }]}
            maxTicks={detailGranularity === 'hour' ? 10 : 14}
            formatYTick={detailField === 'requests' ? formatIntComma : undefined}
          />
        )}
      </div>
    </div>
  );
}
