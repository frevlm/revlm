import type { ReactNode } from 'react';

export type TableProps = {
  /** A `<thead>`'s worth of rows. Rendered on the muted header surface. */
  head?: ReactNode;
  children: ReactNode;
  /** Tighter row height, for dense read-only tables. */
  density?: 'normal' | 'compact';
  /**
   * Lets the table shrink to fit instead of scrolling sideways. Wide tables
   * scroll by default; a table whose columns can wrap should not.
   */
  fit?: boolean;
};

/**
 * The table chrome: scroll container, table element, header surface.
 *
 * Rows and cells stay as plain `<tr>` / `<td>` at the call site on purpose.
 * The console's tables carry expandable detail rows, section dividers and
 * varying colspans; a column-configuration API would have to grow an escape
 * hatch for each of those, and the escape hatch would become the interface.
 */
export function Table({ head, children, density = 'normal', fit }: TableProps) {
  const tableClassName = [
    'table',
    'table-hover',
    'align-middle',
    'mb-0',
    density === 'compact' ? 'table-sm' : '',
    fit ? 'rlm-table-fit' : '',
  ]
    .filter(Boolean)
    .join(' ');

  return (
    <div className={fit ? 'table-responsive rlm-table-responsive-no-x' : 'table-responsive'}>
      <table className={tableClassName}>
        {head ? <thead className="table-light">{head}</thead> : null}
        <tbody>{children}</tbody>
      </table>
    </div>
  );
}
