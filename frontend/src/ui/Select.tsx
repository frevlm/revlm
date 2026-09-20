import type { SelectHTMLAttributes } from 'react';

export type SelectProps = Omit<SelectHTMLAttributes<HTMLSelectElement>, 'className' | 'size'> & {
  size?: 'sm' | 'md';
};

export function Select({ size = 'md', ...rest }: SelectProps) {
  const className = ['form-select', size === 'sm' ? 'form-select-sm' : ''].filter(Boolean).join(' ');
  return <select className={className} {...rest} />;
}
