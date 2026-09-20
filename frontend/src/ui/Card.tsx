import type { ReactNode } from 'react';

export type CardProps = {
  children: ReactNode;
  /** Body padding. `none` lets a table or list run to the card's edges. */
  padding?: 'none' | 'normal' | 'roomy';
  /** Header strip above the body. */
  header?: ReactNode;
  /** Stretches to the tallest sibling in a grid row. */
  fullHeight?: boolean;
  /** Dashed outline, used for the empty/placeholder cards. */
  dashed?: boolean;
  /**
   * `center` stacks the body's content in the middle of the card. Placeholder
   * and call-to-action cards need it; it only reads as centred when the card is
   * also stretched by its row, so it pairs with `fullHeight`.
   */
  align?: 'start' | 'center';
};

const bodyPadding: Record<NonNullable<CardProps['padding']>, string> = {
  none: 'p-0',
  normal: '',
  roomy: 'p-4',
};

/**
 * A bordered surface.
 *
 * Emits no outer margin. The stylesheet gives `.card` a 1.25rem bottom margin
 * and roughly twenty call sites cancel it with `mb-0` — a default that is wrong
 * often enough to be patched everywhere is not a default. Spacing between cards
 * belongs to whatever lays them out (`SegmentedFrame`, `DividedStack`, a grid
 * row's gutters).
 */
export function Card({ children, padding = 'normal', header, fullHeight, dashed, align = 'start' }: CardProps) {
  const className = ['card', 'mb-0', fullHeight ? 'h-100' : '', dashed ? 'border-dashed' : '']
    .filter(Boolean)
    .join(' ');

  return (
    <div className={className}>
      {header ? <div className="card-header">{header}</div> : null}
      <div
        className={[
          'card-body',
          bodyPadding[padding],
          align === 'center' ? 'd-flex flex-column align-items-center justify-content-center text-center' : '',
        ]
          .filter(Boolean)
          .join(' ')}
      >
        {children}
      </div>
    </div>
  );
}
