import type { ReactNode } from 'react';

export type AlertTone = 'info' | 'success' | 'warning' | 'danger';

export type AlertProps = {
  tone: AlertTone;
  children: ReactNode;
  /** A leading status icon, vertically centred against the message. */
  icon?: ReactNode;
  /** Tighter vertical padding, for alerts inside a form or a table cell. */
  compact?: boolean;
};

const toneClass: Record<AlertTone, string> = {
  info: 'alert-info',
  success: 'alert-success',
  warning: 'alert-warning',
  danger: 'alert-danger',
};

/**
 * An inline status message.
 *
 * Emits no margin: every call site currently appends its own `mb-0` / `mb-2` /
 * `mb-3`, which means the framework's default was never the right one. Spacing
 * is the surrounding layout's business.
 */
export function Alert({ tone, children, icon, compact }: AlertProps) {
  const className = ['alert', toneClass[tone], 'mb-0', compact ? 'py-2' : '', icon ? 'd-flex align-items-center' : '']
    .filter(Boolean)
    .join(' ');

  return (
    <div className={className} role="alert">
      {icon ? <span className="me-2 d-inline-flex">{icon}</span> : null}
      {icon ? <div>{children}</div> : children}
    </div>
  );
}
