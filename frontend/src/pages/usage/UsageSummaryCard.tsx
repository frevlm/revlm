import type { UsageWindow } from '../../api/usage';
import { formatSecondsFromMilliseconds } from '../../format/duration';
import { formatIntComma } from '../../format/int';
import { formatUSDPlain } from '../../format/money';
import { cacheHitRate } from './usageUtils';

export function UsageSummaryCard({
  data,
  rangeSinceText,
  rangeUntilText,
}: {
  data: UsageWindow;
  rangeSinceText: string;
  rangeUntilText: string;
}) {
  const rpm = formatIntComma(data.rpm ?? 0);
  const tpm = formatIntComma(data.tpm ?? 0);
  const cachedTotal = data.cache_read_tokens + data.cache_creation_tokens;
  const tokensPerSecond = data.tokens_per_second > 0 ? data.tokens_per_second.toFixed(2) : '-';

  return (
    <div className="card mb-0">
      <div className="card-header d-flex flex-wrap align-items-center justify-content-between gap-2">
        <span>统计区间</span>
        <span className="text-muted smaller fw-normal font-monospace">
          {rangeSinceText} ~ {rangeUntilText}
        </span>
      </div>
      <div className="card-body">
        <div className="rlm-stats">
          <div className="rlm-stat rlm-stat-green">
            <div className="rlm-stat-label">消耗</div>
            <div className="rlm-stat-value">$ {formatUSDPlain(data.usd)}</div>
            <div className="rlm-stat-delta">USD · 含缓存计费</div>
          </div>
          <div className="rlm-stat rlm-stat-blue">
            <div className="rlm-stat-label">请求数</div>
            <div className="rlm-stat-value">{formatIntComma(data.requests)}</div>
            <div className="rlm-stat-delta">RPM {rpm} 次/分钟</div>
          </div>
          <div className="rlm-stat rlm-stat-clay">
            <div className="rlm-stat-label">Token 吞吐</div>
            <div className="rlm-stat-value">{formatIntComma(data.tokens)}</div>
            <div className="rlm-stat-delta">TPM {tpm} Tokens/分钟</div>
          </div>
          <div className="rlm-stat rlm-stat-violet">
            <div className="rlm-stat-label">缓存率</div>
            <div className="rlm-stat-value">{cacheHitRate(data.cache_ratio)}</div>
            <div className="rlm-stat-delta">缓存 Token {formatIntComma(cachedTotal)}</div>
          </div>
        </div>

        <div className="rlm-substats">
          <div className="rlm-substat">
            <div className="rlm-substat-label">输入总计</div>
            <div className="rlm-substat-value">{formatIntComma(data.input_tokens)}</div>
          </div>
          <div className="rlm-substat">
            <div className="rlm-substat-label">输出总计</div>
            <div className="rlm-substat-value">{formatIntComma(data.output_tokens)}</div>
          </div>
          <div className="rlm-substat">
            <div className="rlm-substat-label">缓存 Token</div>
            <div className="rlm-substat-value">{formatIntComma(cachedTotal)}</div>
          </div>
          <div className="rlm-substat">
            <div className="rlm-substat-label">平均首字延迟</div>
            <div className="rlm-substat-value">{formatSecondsFromMilliseconds(data.avg_first_token_latency)}</div>
          </div>
          <div className="rlm-substat">
            <div className="rlm-substat-label">平均 Tokens/s</div>
            <div className="rlm-substat-value">{tokensPerSecond}</div>
          </div>
        </div>
      </div>
    </div>
  );
}
