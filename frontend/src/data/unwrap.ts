import axios from 'axios';

import type { APIResponse } from '../api/types';

/**
 * Pulls the human-readable reason out of whatever axios threw. A non-2xx
 * response still carries the console's `{ success, message }` envelope, so
 * prefer that text over axios's own "Request failed with status code 500".
 */
function reasonFor(error: unknown, fallback: string): string {
  if (axios.isAxiosError(error)) {
    const envelope = error.response?.data as APIResponse<unknown> | undefined;
    if (envelope && typeof envelope.message === 'string' && envelope.message) {
      return envelope.message;
    }
  }
  return error instanceof Error && error.message ? error.message : fallback;
}

/**
 * Collapses the two ways a console request can fail into one.
 *
 * Every endpoint answers with `{ success, message?, data? }`, and axios throws
 * on non-2xx before that envelope is ever inspected — so every call site used
 * to carry both a `try/catch` and an `if (!res.success)` branch. Here they
 * become a single rule: return the envelope, or throw an `Error` whose
 * `message` is the text the UI should show. Nothing above this line branches
 * on `success`, and no component branches on the shape of an error.
 */
async function settle<T>(call: Promise<APIResponse<T>>, fallback: string): Promise<APIResponse<T>> {
  let res: APIResponse<T>;
  try {
    res = await call;
  } catch (error) {
    throw new Error(reasonFor(error, fallback));
  }
  if (!res.success) throw new Error(res.message || fallback);
  return res;
}

/**
 * The response payload.
 *
 * A successful response with no `data` is treated as a failure, because the
 * callers of this helper are queries and a query that resolves with
 * `undefined` is not representable. Endpoints whose success carries no payload
 * use `unwrapOk` or `unwrapMessage` instead.
 */
export async function unwrap<T>(call: Promise<APIResponse<T>>, fallback: string): Promise<T> {
  const res = await settle(call, fallback);
  if (res.data === undefined || res.data === null) throw new Error(res.message || fallback);
  return res.data;
}

/** For endpoints whose success carries nothing the caller needs. */
export async function unwrapOk(call: Promise<APIResponse<unknown>>, fallback: string): Promise<void> {
  await settle(call, fallback);
}

/**
 * For endpoints whose success message is itself the result — "已启用，重启后生效"
 * against "已卸载", rather than a fixed confirmation the caller could have
 * written locally. Failure and success need different fallbacks: one is the
 * reason nothing happened, the other is what happened.
 */
export async function unwrapMessage(
  call: Promise<APIResponse<unknown>>,
  fallback: { error: string; success: string }
): Promise<string> {
  const res = await settle(call, fallback.error);
  return res.message || fallback.success;
}
