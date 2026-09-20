import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import {
  addAdminChannelGroupChannelMember,
  createAdminChannelGroup,
  deleteAdminChannelGroup,
  deleteAdminChannelGroupChannelMember,
  getAdminChannelGroupDetail,
  getAdminChannelGroupPointer,
  listAdminChannelGroups,
  reorderAdminChannelGroupMembers,
  setAdminDefaultChannelGroup,
  updateAdminChannelGroup,
  upsertAdminChannelGroupPointer,
  type AdminChannelGroup,
  type AdminChannelGroupDetail,
  type AdminChannelGroupPointer,
  type CreateAdminChannelGroupRequest,
  type UpdateAdminChannelGroupRequest,
} from '../api/admin/channelGroups';
import { unwrap, unwrapOk } from './unwrap';
import { channelKeys } from './channels';

export const channelGroupKeys = {
  all: ['channelGroups'] as const,
  list: () => ['channelGroups', 'list'] as const,
  detail: (groupID: number) => ['channelGroups', 'detail', groupID] as const,
  pointer: (groupID: number) => ['channelGroups', 'pointer', groupID] as const,
};

export function useChannelGroups() {
  return useQuery<AdminChannelGroup[]>({
    queryKey: channelGroupKeys.list(),
    queryFn: () => unwrap(listAdminChannelGroups(), '加载渠道组失败'),
  });
}

export function useChannelGroupDetail(groupID: number | null) {
  return useQuery<AdminChannelGroupDetail>({
    queryKey: channelGroupKeys.detail(groupID ?? 0),
    enabled: groupID !== null && groupID > 0,
    queryFn: () => unwrap(getAdminChannelGroupDetail(groupID as number), '加载详情失败'),
  });
}

/**
 * A group's pointer is optional: a group with nothing pinned yet answers with
 * `success: false` rather than an error. That "no pointer" case, and an actual
 * request failure, both resolve to `null` here — the editor shows "未设置"
 * either way rather than blocking the rest of the group's detail on it.
 */
export function useChannelGroupPointer(groupID: number | null) {
  return useQuery<AdminChannelGroupPointer | null>({
    queryKey: channelGroupKeys.pointer(groupID ?? 0),
    enabled: groupID !== null && groupID > 0,
    queryFn: async () => {
      try {
        const res = await getAdminChannelGroupPointer(groupID as number);
        return res.success ? res.data || null : null;
      } catch {
        return null;
      }
    },
  });
}

/**
 * Repoints a group at a channel. Both resources go stale: the group now names a
 * different pointer channel, and the channel list shows which groups point at
 * each channel.
 */
export function useSetChannelGroupPointer() {
  const client = useQueryClient();
  return useMutation({
    mutationFn: (vars: { groupID: number; channelID: number; pinned?: boolean }) =>
      unwrapOk(
        upsertAdminChannelGroupPointer(vars.groupID, { channel_id: vars.channelID, pinned: vars.pinned }),
        '设置指针失败'
      ),
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: channelGroupKeys.all });
      void client.invalidateQueries({ queryKey: channelKeys.all });
    },
  });
}

/**
 * Every group mutation below invalidates the whole `channelGroups` subtree —
 * the list row (name/status/pointer badge) and the open detail/pointer queries
 * are different endpoints answering about the same edit, so there is no single
 * cache entry that "owns" the truth to patch in place.
 */
function useChannelGroupMutation<TVars>(run: (vars: TVars) => Promise<void>) {
  const client = useQueryClient();
  return useMutation({
    mutationFn: run,
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: channelGroupKeys.all });
    },
  });
}

export function useCreateChannelGroup() {
  const client = useQueryClient();
  return useMutation({
    mutationFn: (req: CreateAdminChannelGroupRequest) => unwrap(createAdminChannelGroup(req), '创建失败'),
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: channelGroupKeys.all });
    },
  });
}

export function useUpdateChannelGroup() {
  return useChannelGroupMutation((vars: { groupID: number; req: UpdateAdminChannelGroupRequest }) =>
    unwrapOk(updateAdminChannelGroup(vars.groupID, vars.req), '保存失败')
  );
}

export function useDeleteChannelGroup() {
  return useChannelGroupMutation((groupID: number) => unwrapOk(deleteAdminChannelGroup(groupID), '删除失败'));
}

export function useSetDefaultChannelGroup() {
  return useChannelGroupMutation((groupID: number) => unwrapOk(setAdminDefaultChannelGroup(groupID), '设置失败'));
}

export function useAddChannelGroupMember() {
  return useChannelGroupMutation((vars: { groupID: number; channelID: number }) =>
    unwrapOk(addAdminChannelGroupChannelMember(vars.groupID, vars.channelID), '添加失败')
  );
}

export function useRemoveChannelGroupMember() {
  return useChannelGroupMutation((vars: { groupID: number; channelID: number }) =>
    unwrapOk(deleteAdminChannelGroupChannelMember(vars.groupID, vars.channelID), '移除失败')
  );
}

export function useReorderChannelGroupMembers() {
  return useChannelGroupMutation((vars: { groupID: number; memberIDs: number[] }) =>
    unwrapOk(reorderAdminChannelGroupMembers(vars.groupID, vars.memberIDs), '保存排序失败')
  );
}
