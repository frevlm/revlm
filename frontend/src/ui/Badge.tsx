import type { ReactNode } from 'react';

export type BadgeTone = 'neutral' | 'primary' | 'info' | 'success' | 'warning' | 'danger';

export type BadgeProps = {
  tone?: BadgeTone;
  children: ReactNode;
  /** Fully rounded ends. Used for state chips; square ones read as labels. */
  pill?: boolean;
  title?: string;
};

// Tinted fill plus matching text and border. `neutral` is the odd one out
// because there is no `-subtle` variable for it.
const toneClass: Record<BadgeTone, string> = {
  neutral: 'bg-light text-secondary border',
  primary: 'bg-primary-subtle text-primary border border-primary-subtle',
  info: 'bg-info-subtle text-info border border-info-subtle',
  success: 'bg-success-subtle text-success border border-success-subtle',
  warning: 'bg-warning-subtle text-warning-emphasis border border-warning-subtle',
  danger: 'bg-danger-subtle text-danger border border-danger-subtle',
};

export function Badge({ tone = 'neutral', children, pill, title }: BadgeProps) {
  const className = ['badge', toneClass[tone], 'fw-normal', pill ? 'rounded-pill px-2' : ''].filter(Boolean).join(' ');
  return (
    <span className={className} title={title}>
      {children}
    </span>
  );
}
