import { useEffect, useRef, useState, type ComponentPropsWithoutRef, type ElementType, type ReactNode } from 'react';

export type MenuProps = {
  /** The clickable element that opens the menu. */
  trigger: ReactNode;
  children: ReactNode;
  /** Which edge the panel lines up with. */
  align?: 'start' | 'end';
  label?: string;
};

/**
 * A menu anchored to a trigger.
 *
 * Controlled by React state rather than by the framework's `data-bs-toggle`
 * data-api, so the panel's open state is visible to the component tree and the
 * menu can be rendered more than once on a page. The framework's `.dropdown`
 * CSS still positions and animates it.
 */
export function Menu({ trigger, children, align = 'end', label }: MenuProps) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!open) return;
    const onPointerDown = (event: MouseEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false);
    };
    document.addEventListener('mousedown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('mousedown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
    };
  }, [open]);

  return (
    <div className="dropdown" ref={rootRef}>
      <button
        type="button"
        className="btn btn-link p-0 text-decoration-none d-flex align-items-center text-body dropdown-toggle"
        aria-expanded={open}
        aria-haspopup="menu"
        aria-label={label}
        onClick={() => setOpen((value) => !value)}
      >
        {trigger}
      </button>
      <ul
        className={[
          'dropdown-menu',
          align === 'end' ? 'dropdown-menu-end' : '',
          'border-0 shadow-lg mt-2 p-2 rounded-4',
          open ? 'show' : '',
        ]
          .filter(Boolean)
          .join(' ')}
        role="menu"
      >
        {children}
      </ul>
    </div>
  );
}

export function MenuHeader({ children }: { children: ReactNode }) {
  return (
    <li>
      <div className="dropdown-header">{children}</div>
    </li>
  );
}

export function MenuDivider() {
  return (
    <li>
      <hr className="dropdown-divider" />
    </li>
  );
}

type MenuItemOwnProps = {
  tone?: 'default' | 'danger';
  children: ReactNode;
};

/**
 * `component` lets a caller render the item as a router link (or anything else)
 * without this file importing a router — the primitive owns the class name, the
 * caller owns the element.
 */
export function MenuItem<TComponent extends ElementType = 'button'>({
  component,
  tone = 'default',
  ...rest
}: MenuItemOwnProps & { component?: TComponent } & Omit<
    ComponentPropsWithoutRef<TComponent>,
    keyof MenuItemOwnProps | 'component'
  >) {
  const Component = (component ?? 'button') as ElementType;
  const className = ['dropdown-item', 'rounded-2', tone === 'danger' ? 'text-danger' : ''].filter(Boolean).join(' ');
  const typeProp = Component === 'button' ? { type: 'button' as const } : {};
  return (
    <li role="none">
      <Component className={className} role="menuitem" {...typeProp} {...rest} />
    </li>
  );
}
