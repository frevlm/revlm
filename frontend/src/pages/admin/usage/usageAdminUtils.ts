export type UsageAdminDetailField = 'usd' | 'requests' | 'avg_first_token_latency';

export type UsageAdminDetailGranularity = 'hour' | 'day';

export const usageAdminFieldOptions: Array<{
  value: UsageAdminDetailField;
  label: string;
}> = [
  { value: 'usd', label: '消耗 (USD)' },
  { value: 'requests', label: '请求数' },
  { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
];

export const usageAdminGranularityOptions: Array<{
  value: UsageAdminDetailGranularity;
  label: string;
}> = [
  { value: 'hour', label: '按小时' },
  { value: 'day', label: '按天' },
];

export function formatDecimalPlain(raw: string | number | null | undefined): string {
  let value = (raw ?? '').toString().trim();
  if (!value) return '0';
  if (value.startsWith('+')) value = value.slice(1).trim();
  if (value.startsWith('$')) value = value.slice(1).trim();
  if (!value) return '0';
  if (value.includes('.')) {
    value = value.replace(/0+$/, '').replace(/\.$/, '');
  }
  if (value === '-0' || value === '') return '0';
  return value;
}

export function formatUSD(raw: string): string {
  const normalized = formatDecimalPlain(raw);
  if (normalized.startsWith('-')) return `-$${normalized.slice(1)}`;
  return `$${normalized}`;
}
