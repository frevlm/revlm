import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import {
  createUserToken,
  deleteUserToken,
  getUserTokenChannel,
  listUserTokens,
  revealUserToken,
  revokeUserToken,
  rotateUserToken,
  setUserTokenChannel,
  type UserToken,
  type UserTokenChannelGroup,
} from '../api/tokens';
import { getUsageWindows } from '../api/usage';
import { unwrap, unwrapOk } from './unwrap';

export const tokenKeys = {
  all: ['tokens'] as const,
  list: () => ['tokens', 'list'] as const,
  channel: (tokenID: number) => ['tokens', 'channel', tokenID] as const,
  usageWindow: (tokenID: number, range: TokenUsageRange) => ['tokens', 'usageWindow', tokenID, range] as const,
};

/** The window the usage modal has committed — not what is currently typed. */
export type TokenUsageRange = { start: string; end: string };

const tokensQuery = {
  queryKey: tokenKeys.list(),
  queryFn: () => unwrap(listUserTokens(), '加载失败'),
};

export function useTokens() {
  return useQuery<UserToken[]>(tokensQuery);
}

/**
 * The same cache entry as `useTokens`, reshaped as an id lookup for tables that
 * only need to name a token. Sharing the key matters: a second key over the
 * same endpoint would mean two copies of the token list with two independent
 * invalidation stories, which is the defect this layer exists to remove.
 */
export function useTokensByID() {
  return useQuery<UserToken[], Error, Record<number, UserToken>>({
    ...tokensQuery,
    select: (list) => {
      const byID: Record<number, UserToken> = {};
      for (const token of list) byID[token.id] = token;
      return byID;
    },
  });
}

/**
 * Every token mutation invalidates the whole `tokens` subtree — the list and
 * any open channel-assignment query alike — rather than patching cached
 * entries in place, matching the channels resource (`src/data/channels.ts`).
 */
function useTokenMutation<TVars, TData>(mutationFn: (vars: TVars) => Promise<TData>) {
  const client = useQueryClient();
  return useMutation({
    mutationFn,
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: tokenKeys.all });
    },
  });
}

export function useCreateToken() {
  return useTokenMutation((name: string | undefined) => unwrap(createUserToken(name), '创建失败'));
}

export function useRotateToken() {
  return useTokenMutation((tokenID: number) => unwrap(rotateUserToken(tokenID), '重新生成失败'));
}

export function useRevokeToken() {
  return useTokenMutation((tokenID: number) => unwrapOk(revokeUserToken(tokenID), '撤销失败'));
}

export function useDeleteToken() {
  return useTokenMutation((tokenID: number) => unwrapOk(deleteUserToken(tokenID), '删除失败'));
}

/**
 * Reveals one token's plaintext. Unlike the other mutations this never
 * invalidates the list: revealing doesn't change anything server-side, it
 * only pulls a secret the list query never carries.
 */
export function useRevealToken() {
  return useMutation({
    mutationFn: async (tokenID: number) => {
      const data = await unwrap(revealUserToken(tokenID), '查看失败');
      const tok = (data.token || '').toString();
      if (tok.trim() === '') throw new Error('查看失败');
      return tok;
    },
  });
}

async function fetchTokenChannel(tokenID: number) {
  return unwrap(getUserTokenChannel(tokenID), '加载失败');
}

export function useTokenChannel(tokenID: number | null) {
  return useQuery<UserTokenChannelGroup>({
    queryKey: tokenKeys.channel(tokenID ?? 0),
    enabled: tokenID !== null,
    queryFn: () => fetchTokenChannel(tokenID as number),
  });
}

/**
 * Imperative twin of `useTokenChannel`, for the one caller that must hold the
 * response before it proceeds: opening the channel modal defers showing it
 * until the fetch settles (success or failure), which a reactively-rendered
 * `useQuery` can't express. Both share the same cache entry, so once the
 * modal is open `useTokenChannel` reads what this already fetched.
 */
export function useFetchTokenChannel() {
  const client = useQueryClient();
  return (tokenID: number) =>
    client.fetchQuery({ queryKey: tokenKeys.channel(tokenID), queryFn: () => fetchTokenChannel(tokenID) });
}

export function useSetTokenChannel() {
  return useTokenMutation((vars: { tokenID: number; channelGroupID: number }) =>
    unwrapOk(setUserTokenChannel(vars.tokenID, vars.channelGroupID), '保存失败')
  );
}

/**
 * One token's usage over a window. An empty range means "let the server pick",
 * and the server echoes back the window it chose — the caller displays that
 * echo rather than writing it into the range inputs, so the committed range
 * stays a pure record of what the user asked for.
 */
export function useTokenUsageWindow(tokenID: number | null, range: TokenUsageRange) {
  return useQuery({
    queryKey: tokenKeys.usageWindow(tokenID ?? 0, range),
    enabled: tokenID !== null,
    queryFn: () =>
      unwrap(getUsageWindows(range.start || undefined, range.end || undefined, tokenID as number), '加载失败'),
  });
}
