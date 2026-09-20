import type { InputHTMLAttributes } from 'react';

export type InputProps = Omit<InputHTMLAttributes<HTMLInputElement>, 'className' | 'size'> & {
  size?: 'sm' | 'md';
  /** Monospace text, for keys, URLs and ids where character shape matters. */
  mono?: boolean;
  /** Recessed background, for read-mostly fields shown next to a copy button. */
  recessed?: boolean;
};

export function Input({ size = 'md', mono, recessed, ...rest }: InputProps) {
  const className = [
    'form-control',
    size === 'sm' ? 'form-control-sm' : '',
    mono ? 'font-monospace' : '',
    recessed ? 'bg-light' : '',
  ]
    .filter(Boolean)
    .join(' ');
  return <input className={className} {...rest} />;
}
