import { useEffect, useState } from 'react';

/**
 * Trails `value` by `delayMs`, so a fast-changing input (a date range being
 * typed, a search box) can drive a query key without firing a request per
 * keystroke. The debounce belongs to the input, not to the resource: the data
 * layer stays a pure function of the committed value.
 */
export function useDebouncedValue<T>(value: T, delayMs: number): T {
  const [settled, setSettled] = useState(value);

  useEffect(() => {
    const timer = window.setTimeout(() => setSettled(value), delayMs);
    return () => window.clearTimeout(timer);
  }, [value, delayMs]);

  return settled;
}
