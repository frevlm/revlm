import type { ComponentPropsWithoutRef, ElementType, ReactNode } from 'react';

/**
 * How much visual weight the button carries.
 *
 * `soft` and `quiet` differ only by a border, which reads like drift but is the
 * console's actual convention today: `soft` is a neutral action sitting on a
 * card or toolbar, `quiet` is the low-emphasis dismissal in a modal footer. The
 * two collapse into one once the framework's button CSS is replaced by tokens.
 */
export type ButtonVariant = 'solid' | 'soft' | 'quiet' | 'outline' | 'link';

/** What the action means. Independent of weight — a delete can be soft or solid. */
export type ButtonTone = 'default' | 'primary' | 'success' | 'warning' | 'danger';

type ButtonOwnProps = {
  variant?: ButtonVariant;
  tone?: ButtonTone;
  size?: 'sm' | 'md';
  /** Extra horizontal padding, used by the confirming button of a modal footer. */
  wide?: boolean;
  children?: ReactNode;
};

/**
 * `component` renders the button as something else — a router `Link` for an
 * action that navigates — without this file importing a router. The primitive
 * keeps the class names; the caller chooses the element.
 */
export type ButtonProps<TComponent extends ElementType = 'button'> = ButtonOwnProps & {
  component?: TComponent;
} & Omit<ComponentPropsWithoutRef<TComponent>, keyof ButtonOwnProps | 'component' | 'className'>;

const solidTone: Record<ButtonTone, string> = {
  default: 'btn-secondary',
  primary: 'btn-primary',
  success: 'btn-success text-white',
  warning: 'btn-warning',
  danger: 'btn-danger',
};

const outlineTone: Record<ButtonTone, string> = {
  default: 'btn-outline-secondary',
  primary: 'btn-outline-primary',
  success: 'btn-outline-success',
  warning: 'btn-outline-warning',
  danger: 'btn-outline-danger',
};

// `soft`, `quiet` and `link` all render a neutral surface and carry their tone
// as text color rather than as a fill.
const textTone: Record<ButtonTone, string> = {
  default: '',
  primary: 'text-primary',
  success: 'text-success',
  warning: 'text-warning',
  danger: 'text-danger',
};

function classesFor(variant: ButtonVariant, tone: ButtonTone): string {
  switch (variant) {
    case 'solid':
      return solidTone[tone];
    case 'outline':
      return outlineTone[tone];
    case 'soft':
      return `btn-light border ${textTone[tone]}`;
    case 'quiet':
      return `btn-light ${textTone[tone]}`;
    case 'link':
      return `btn-link p-0 text-decoration-none ${textTone[tone] || 'text-secondary'}`;
  }
}

/**
 * The console's only button. Callers name intent (`variant`, `tone`, `size`);
 * this file is the one place framework class names for buttons appear.
 *
 * There is deliberately no `className` escape hatch: reopening it would put the
 * class strings back in the pages, which is the state this layer exists to end.
 * A one-off that genuinely does not fit belongs in its own component next to the
 * feature that needs it, where it is visible as an exception.
 */
export function Button<TComponent extends ElementType = 'button'>({
  variant = 'soft',
  tone = 'default',
  size = 'md',
  wide,
  component,
  ...rest
}: ButtonProps<TComponent>) {
  const Component = (component ?? 'button') as ElementType;
  const className = [
    'btn',
    classesFor(variant, tone),
    size === 'sm' ? (variant === 'link' ? 'small' : 'btn-sm') : '',
    wide ? 'px-4' : '',
  ]
    .join(' ')
    .replace(/\s+/g, ' ')
    .trim();

  // A bare <button> inside a form submits by default; almost every call site
  // wants a plain button, and the ones that submit pass type="submit".
  const typeProp = Component === 'button' && !('type' in rest) ? { type: 'button' as const } : {};
  return <Component className={className} {...typeProp} {...rest} />;
}
