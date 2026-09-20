import { useEffect, useId, useRef, type ReactNode } from 'react';
import { createPortal } from 'react-dom';

import { usePresence } from '../hooks/usePresence';
import { Button } from './Button';

export type ModalProps = {
  open: boolean;
  onClose: () => void;
  title: ReactNode;
  children: ReactNode;
  size?: 'md' | 'lg';
  /** Scrolls the body instead of the page when the content is taller than the viewport. */
  scrollable?: boolean;
  /** Recessed body surface, used by the large editing forms. */
  bodyTone?: 'plain' | 'muted';
  footer?: ReactNode;
  closeLabel?: string;
};

// Bootstrap's `.modal-open` rule locks the page behind the dialog. Two modals
// can be present at once during a cross-fade, so the lock is refcounted —
// otherwise the one closing releases the scroll the one opening still needs.
let openModalCount = 0;

function lockPageScroll() {
  openModalCount += 1;
  if (openModalCount === 1) document.body.classList.add('modal-open');
  return () => {
    openModalCount -= 1;
    if (openModalCount === 0) document.body.classList.remove('modal-open');
  };
}

/** Matches the framework's modal transition, so enter/leave stay in step with its CSS. */
const TRANSITION_MS = 300;

/**
 * A controlled dialog: `open` is a prop, not a DOM id.
 *
 * The console used to open and close modals by `document.getElementById(id)`
 * and by synthesizing a click on a `data-bs-dismiss` button. That made the id
 * the identity of the dialog, so rendering the same modal twice on one page —
 * exactly what a component gallery does — collided. Here the dialog exists only
 * while `open`, and closing is a callback.
 *
 * Only the framework's JavaScript is dropped, not its CSS: `.modal`, `.fade`
 * and `.show` still do the animating, driven by `usePresence` instead of by
 * `bootstrap.Modal`.
 */
export function Modal({
  open,
  onClose,
  title,
  children,
  size = 'md',
  scrollable,
  bodyTone = 'plain',
  footer,
  closeLabel = '关闭',
}: ModalProps) {
  const titleId = useId();
  const dialogRef = useRef<HTMLDivElement | null>(null);
  const { present, phase } = usePresence(open, TRANSITION_MS);

  useEffect(() => {
    if (!present) return;
    return lockPageScroll();
  }, [present]);

  useEffect(() => {
    if (!open) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose();
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
  }, [open, onClose]);

  useEffect(() => {
    if (open) dialogRef.current?.focus();
  }, [open]);

  if (!present || typeof document === 'undefined') return null;

  const shown = phase === 'enter';
  const dialogClassName = [
    'modal-dialog',
    'modal-dialog-centered',
    size === 'lg' ? 'modal-lg' : '',
    scrollable ? 'modal-dialog-scrollable' : '',
  ]
    .filter(Boolean)
    .join(' ');

  return createPortal(
    <>
      <div className={`modal-backdrop fade${shown ? ' show' : ''}`} />
      <div
        className={`modal fade${shown ? ' show' : ''}`}
        style={{ display: 'block' }}
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        tabIndex={-1}
        ref={dialogRef}
        // mousedown, not click: a drag that starts inside the dialog and ends on
        // the backdrop is a text selection, not a dismissal.
        onMouseDown={(event) => {
          if (event.target === event.currentTarget) onClose();
        }}
      >
        <div className={dialogClassName}>
          <div className="modal-content border-0 shadow">
            <div className="modal-header">
              <h5 className="modal-title fw-bold" id={titleId}>
                {title}
              </h5>
              <button type="button" className="btn-close" aria-label={closeLabel} onClick={onClose}></button>
            </div>
            <div className={`modal-body${bodyTone === 'muted' ? ' bg-light' : ''}`}>{children}</div>
            {footer ? <div className="modal-footer border-top-0">{footer}</div> : null}
          </div>
        </div>
      </div>
    </>,
    document.body
  );
}

/** The standard dismissing button for a modal footer. */
export function ModalCloseButton({ onClose, children = '取消' }: { onClose: () => void; children?: ReactNode }) {
  return (
    <Button variant="quiet" onClick={onClose}>
      {children}
    </Button>
  );
}
