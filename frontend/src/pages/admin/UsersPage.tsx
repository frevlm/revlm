import { useState, type FormEvent } from 'react';

import { useQueryClient } from '@tanstack/react-query';

import { useAuth } from '../../auth/AuthContext';
import { type AdminUser } from '../../api/admin/users';
import { useDeleteUser, useUpdateUser, useUsers, userKeys } from '../../data/users';
import { BootstrapModal } from '../../components/BootstrapModal';
import { DividedStack } from '../../components/DividedStack';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { closeModalById } from '../../components/modal';
import { ConfigForm } from '../../components/admin/ConfigForm';
import {
  addAdminUserBalanceTemplate,
  createAdminUserTemplate,
  resetAdminUserPasswordTemplate,
} from '../../components/admin/configTemplates';

function roleBadge(role: string): string {
  if (role === 'root')
    return 'badge rounded-pill bg-primary bg-opacity-10 text-primary border border-primary border-opacity-25 px-2';
  return 'badge rounded-pill bg-light text-secondary border px-2';
}

function statusBadge(status: number): { cls: string; label: string } {
  if (status === 1) return { cls: 'badge rounded-pill bg-success bg-opacity-10 text-success px-2', label: '启用' };
  return { cls: 'badge rounded-pill bg-secondary bg-opacity-10 text-secondary px-2', label: '禁用' };
}

type EditDraft = { email: string; role: 'user' | 'root'; status: number };

function draftFromUser(user: AdminUser): EditDraft {
  return {
    email: user.email || '',
    role: (user.role || 'user') as 'user' | 'root',
    status: user.status || 0,
  };
}

/**
 * Seeded once per mount from `user`; the caller keys this on `user.id`, so
 * opening a different row's edit modal remounts it with a fresh draft instead
 * of needing an effect to resync one shared draft object.
 */
function EditUserForm({
  user,
  selfID,
  onSubmitStart,
  onError,
  onSaved,
}: {
  user: AdminUser;
  selfID: number;
  onSubmitStart: () => void;
  onError: (message: string) => void;
  onSaved: () => void;
}) {
  const [draft, setDraft] = useState<EditDraft>(() => draftFromUser(user));
  const updateUser = useUpdateUser();

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    onSubmitStart();
    try {
      await updateUser.mutateAsync({
        userID: user.id,
        req: { email: draft.email.trim(), role: draft.role, status: draft.status },
      });
      onSaved();
    } catch (error) {
      onError(error instanceof Error ? error.message : '保存失败');
    }
  }

  return (
    <form className="row g-3" onSubmit={(e) => void handleSubmit(e)}>
      <div className="col-md-6">
        <label className="form-label">邮箱</label>
        <input
          className="form-control"
          value={draft.email}
          onChange={(e) => setDraft((prev) => ({ ...prev, email: e.target.value }))}
          required
        />
        <div className="form-text small text-muted">修改邮箱后，新邮箱会立即用于后续登录。</div>
      </div>
      <div className="col-md-6">
        <label className="form-label">账号名</label>
        <input className="form-control" value={user.username || ''} disabled />
        <div className="form-text small text-muted">账号名不可修改；用于登录（区分大小写，仅字母/数字）。</div>
      </div>
      <div className="col-md-6">
        <label className="form-label">状态</label>
        <select
          className="form-select"
          value={draft.status}
          onChange={(e) => setDraft((prev) => ({ ...prev, status: Number.parseInt(e.target.value, 10) || 0 }))}
          disabled={user.id === selfID}
        >
          <option value={1}>启用</option>
          <option value={0}>禁用</option>
        </select>
      </div>
      <div className="col-md-6">
        <label className="form-label">角色</label>
        <select
          className="form-select"
          value={draft.role}
          onChange={(e) => setDraft((prev) => ({ ...prev, role: (e.target.value as 'user' | 'root') || 'user' }))}
          disabled={user.id === selfID}
        >
          <option value="user">普通用户</option>
          <option value="root">超级管理员</option>
        </select>
        {user.id === selfID ? (
          <div className="form-text small text-muted">不能修改当前登录用户的状态或角色。</div>
        ) : null}
      </div>
      <div className="modal-footer border-top-0 px-0 pb-0">
        <button type="button" className="btn btn-light" data-bs-dismiss="modal">
          取消
        </button>
        <button className="btn btn-primary px-4" type="submit">
          确认更改
        </button>
      </div>
    </form>
  );
}

export function UsersPage() {
  const { user: self } = useAuth();
  const selfID = self?.id || 0;
  const queryClient = useQueryClient();

  const { data } = useUsers();
  const users = data ?? [];
  const deleteUser = useDeleteUser();

  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [editingID, setEditingID] = useState<number | null>(null);
  const editing = users.find((u) => u.id === editingID) ?? null;

  const enabledCount = users.filter((u) => u.status === 1).length;

  function clearBanners() {
    setErr('');
    setNotice('');
  }

  async function handleDelete(u: AdminUser) {
    if (u.id === selfID) return;
    if (!window.confirm('确认删除该用户？此操作不可恢复。')) return;
    clearBanners();
    try {
      await deleteUser.mutateAsync(u.id);
      setNotice('已删除');
      if (editingID === u.id) setEditingID(null);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '删除失败');
    }
  }

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <div className="card mb-0">
            <div className="card-body d-flex flex-column flex-md-row justify-content-between align-items-center">
              <div className="d-flex align-items-center mb-3 mb-md-0">
                <div
                  className="bg-warning bg-opacity-10 text-warning rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">group</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">用户管理</h5>
                  <p className="mb-0 text-muted small">
                    {enabledCount} 启用 / {users.length} 总计 · 仅 root 可管理用户
                  </p>
                </div>
              </div>

              <div className="d-flex gap-2">
                <button
                  type="button"
                  className="btn btn-primary btn-sm"
                  data-bs-toggle="modal"
                  data-bs-target="#createUserModal"
                >
                  <span className="me-1 material-symbols-rounded">person_add</span> 创建用户
                </button>
              </div>
            </div>
          </div>

          {notice ? (
            <div className="alert alert-success d-flex align-items-center mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">check_circle</span>
              <div>{notice}</div>
            </div>
          ) : null}

          {err ? (
            <div className="alert alert-danger d-flex align-items-center mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">warning</span>
              <div>{err}</div>
            </div>
          ) : null}

          {users.length === 0 ? (
            <div className="text-center py-5 text-muted">
              <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
              暂无用户。
            </div>
          ) : (
            <div className="card overflow-hidden mb-0">
              <div className="table-responsive">
                <table className="table table-hover align-middle mb-0">
                  <thead className="table-light">
                    <tr>
                      <th className="ps-4">邮箱</th>
                      <th>账号名</th>
                      <th>角色</th>
                      <th>状态</th>
                      <th>余额(USD)</th>
                      <th className="text-end pe-4">操作</th>
                    </tr>
                  </thead>
                  <tbody>
                    {users.map((u) => {
                      const st = statusBadge(u.status);
                      return (
                        <tr key={u.id}>
                          <td className="ps-4">
                            <span className="fw-bold text-dark">{u.email}</span>
                          </td>
                          <td>
                            {u.username ? (
                              <span className="text-dark fw-medium user-select-all">{u.username}</span>
                            ) : (
                              <span className="text-muted small fst-italic">未设置</span>
                            )}
                          </td>
                          <td>
                            <span className={roleBadge(u.role)}>{u.role}</span>
                          </td>
                          <td>
                            <span className={st.cls}>{st.label}</span>
                          </td>
                          <td className="fw-medium text-dark">{u.balance_usd}</td>
                          <td className="text-end pe-4 text-nowrap">
                            <div className="d-inline-flex gap-1">
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-success"
                                title="加余额"
                                data-bs-toggle="modal"
                                data-bs-target="#addBalanceModal"
                                onClick={() => setEditingID(u.id)}
                              >
                                <i className="ri-money-dollar-circle-line"></i>
                              </button>
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-primary"
                                title="编辑用户"
                                data-bs-toggle="modal"
                                data-bs-target="#editUserModal"
                                onClick={() => setEditingID(u.id)}
                              >
                                <i className="ri-edit-line"></i>
                              </button>
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-warning"
                                title="重置密码"
                                data-bs-toggle="modal"
                                data-bs-target="#resetPasswordModal"
                                onClick={() => setEditingID(u.id)}
                              >
                                <i className="ri-key-2-line"></i>
                              </button>
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-danger"
                                title={u.id === selfID ? '不能删除当前登录用户' : '删除用户'}
                                disabled={u.id === selfID}
                                onClick={() => void handleDelete(u)}
                              >
                                <i className="ri-delete-bin-line"></i>
                              </button>
                            </div>
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </div>
          )}
        </DividedStack>
      </SegmentedFrame>

      <BootstrapModal id="createUserModal" title="创建用户" dialogClassName="modal-dialog-centered modal-lg">
        <ConfigForm
          template={createAdminUserTemplate}
          onSubmitStart={clearBanners}
          onError={(message) => setErr(message || '创建失败')}
          onSaved={() => {
            setNotice('已创建');
            closeModalById('createUserModal');
            void queryClient.invalidateQueries({ queryKey: userKeys.all });
          }}
          resetOnSaved
          submitClassName="btn btn-primary px-4"
          footerStart={
            <button type="button" className="btn btn-light" data-bs-dismiss="modal">
              取消
            </button>
          }
        />
      </BootstrapModal>

      <BootstrapModal
        id="editUserModal"
        title={editing ? `编辑用户：${editing.email}` : '编辑用户'}
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setEditingID(null);
        }}
      >
        {!editing ? (
          <div className="text-muted">未选择用户。</div>
        ) : (
          <EditUserForm
            key={editing.id}
            user={editing}
            selfID={selfID}
            onSubmitStart={clearBanners}
            onError={setErr}
            onSaved={() => {
              setNotice('已保存');
              closeModalById('editUserModal');
            }}
          />
        )}
      </BootstrapModal>

      <BootstrapModal
        id="addBalanceModal"
        title={editing ? `加余额：${editing.email}` : '加余额'}
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setEditingID(null);
        }}
      >
        {!editing ? (
          <div className="text-muted">未选择用户。</div>
        ) : (
          <>
            <div className="alert alert-light border py-2 small">
              <div className="d-flex justify-content-between">
                <span className="text-muted">当前余额</span>
                <span className="fw-bold text-dark">{editing.balance_usd} USD</span>
              </div>
            </div>
            <ConfigForm
              template={addAdminUserBalanceTemplate(editing.id)}
              onSubmitStart={clearBanners}
              onError={(message) => setErr(message || '加余额失败')}
              onSaved={() => {
                setNotice('已加余额');
                closeModalById('addBalanceModal');
                void queryClient.invalidateQueries({ queryKey: userKeys.all });
              }}
              submitClassName="btn btn-success px-4"
              footerStart={
                <button type="button" className="btn btn-light" data-bs-dismiss="modal">
                  取消
                </button>
              }
            />
          </>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="resetPasswordModal"
        title={editing ? `重置密码：${editing.email}` : '重置密码'}
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setEditingID(null);
        }}
      >
        {!editing ? (
          <div className="text-muted">未选择用户。</div>
        ) : (
          <>
            <ConfigForm
              template={resetAdminUserPasswordTemplate(editing.id)}
              onSubmitStart={clearBanners}
              onError={(message) => setErr(message || '重置失败')}
              onSaved={() => {
                setNotice('已重置密码');
                closeModalById('resetPasswordModal');
                void queryClient.invalidateQueries({ queryKey: userKeys.all });
              }}
              submitClassName="btn btn-primary px-4"
              footerStart={
                <button type="button" className="btn btn-light" data-bs-dismiss="modal">
                  取消
                </button>
              }
            />
          </>
        )}
      </BootstrapModal>
    </div>
  );
}
