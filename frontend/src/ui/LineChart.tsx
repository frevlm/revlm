import { useEffect, useRef } from 'react';
import Chart from 'chart.js/auto';

/**
 * The palette a series can ask for. Callers name an intent; resolving it to a
 * concrete color is this file's business, the same way the other primitives own
 * their class names — so swapping the underlying design system later is a
 * change here and nowhere else.
 */
export type ChartTone = 'primary' | 'info' | 'success' | 'warning' | 'danger' | 'secondary';

export type LineChartSeries = {
  label: string;
  tone: ChartTone;
  values: number[];
};

type LineChartProps = {
  /** X-axis labels; one per index in every series' `values`. */
  labels: string[];
  series: LineChartSeries[];
  title?: string;
  /** Caps x-axis labels so dense hourly series stay readable. */
  maxTicks?: number;
  /** Formats y-axis ticks — e.g. thousands separators for counts. */
  formatYTick?: (value: string | number) => string;
  height?: number;
};

// Fallbacks matter: `getComputedStyle` returns an empty string before the
// stylesheet applies, and a chart with no colors reads as a bug.
const toneFallback: Record<ChartTone, string> = {
  primary: '60, 138, 97',
  info: '53, 90, 96',
  success: '47, 107, 75',
  warning: '122, 98, 50',
  danger: '122, 52, 52',
  secondary: '99, 116, 107',
};

function resolveTones(element: Element) {
  const css = getComputedStyle(element);
  const channels = (tone: ChartTone) => (css.getPropertyValue(`--bs-${tone}-rgb`).trim() || toneFallback[tone]).trim();
  return {
    line: (tone: ChartTone) => `rgba(${channels(tone)}, 0.95)`,
    fill: (tone: ChartTone) => `rgba(${channels(tone)}, 0.18)`,
    grid: `rgba(${channels('secondary')}, 0.18)`,
  };
}

/**
 * A line chart over one or more series sharing an x axis.
 *
 * Owns the Chart.js instance: it is created on mount, destroyed on unmount, and
 * rebuilt whenever the inputs change. Chart.js draws imperatively into a canvas
 * it keeps a handle on, so leaving an old instance alive over a re-rendered
 * canvas leaks both the instance and its resize listener.
 */
export function LineChart({ labels, series, title, maxTicks, formatYTick, height = 280 }: LineChartProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  // Rebuild on content, not on identity. Callers build `labels` and `series`
  // inline from query data, so a reference-based dependency list would tear
  // down and redraw the chart on every render of the page around it. Comparing
  // the rendered content instead costs one JSON pass and needs no memoization
  // discipline at the call site.
  const signature = JSON.stringify([labels, series, title, maxTicks, height]);
  const latest = useRef({ labels, series, title, maxTicks, formatYTick });
  latest.current = { labels, series, title, maxTicks, formatYTick };

  useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas?.getContext('2d');
    if (!canvas || !ctx) return;

    const { labels, series, title, maxTicks, formatYTick } = latest.current;
    const tone = resolveTones(canvas);
    const chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels,
        datasets: series.map((line) => ({
          label: line.label,
          data: line.values,
          borderColor: tone.line(line.tone),
          backgroundColor: tone.fill(line.tone),
          pointRadius: 2,
          tension: 0.2,
        })),
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: { position: 'bottom' },
          title: title ? { display: true, text: title } : { display: false },
        },
        scales: {
          x: {
            grid: { display: false },
            ticks: { autoSkip: true, maxTicksLimit: maxTicks, maxRotation: 0, minRotation: 0 },
          },
          y: {
            beginAtZero: true,
            grid: { color: tone.grid },
            ...(formatYTick ? { ticks: { callback: formatYTick } } : {}),
          },
        },
      },
    });
    chartRef.current = chart;

    return () => {
      chart.destroy();
      chartRef.current = null;
    };
  }, [signature]);

  return (
    <div style={{ height }}>
      <canvas ref={canvasRef}></canvas>
    </div>
  );
}
