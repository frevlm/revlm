import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

import {
  deleteAdminUser,
  listAdminUsers,
  updateAdminUser,
  type AdminUser,
  type UpdateAdminUserRequest,
} from '../api/admin/users';
import { unwrap, unwrapOk } from './unwrap';

export const userKeys = {
  all: ['users'] as const,
  list: () => ['users', 'list'] as const,
};

export function useUsers() {
  return useQuery<AdminUser[]>({
    queryKey: userKeys.list(),
    queryFn: () => unwrap(listAdminUsers(), '加载用户失败'),
  });
}

/**
 * User creation, password reset and balance top-up run through `ConfigForm` /
 * `configTemplates.ts`, which call the transport layer directly rather than
 * through this module. Callers of those forms invalidate `userKeys.all`
 * themselves on `onSaved` — see `UsersPage`.
 */
function useUserMutation<TVars>(run: (vars: TVars) => Promise<void>) {
  const client = useQueryClient();
  return useMutation({
    mutationFn: run,
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: userKeys.all });
    },
  });
}

export function useUpdateUser() {
  return useUserMutation((vars: { userID: number; req: UpdateAdminUserRequest }) =>
    unwrapOk(updateAdminUser(vars.userID, vars.req), '保存失败')
  );
}

export function useDeleteUser() {
  return useUserMutation((userID: number) => unwrapOk(deleteAdminUser(userID), '删除失败'));
}
