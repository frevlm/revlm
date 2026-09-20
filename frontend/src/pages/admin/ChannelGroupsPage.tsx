import { useState } from 'react';

import { type AdminChannelGroup, type AdminChannelGroupMember } from '../../api/admin/channelGroups';
import {
  useAddChannelGroupMember,
  useChannelGroupDetail,
  useChannelGroupPointer,
  useChannelGroups,
  useCreateChannelGroup,
  useDeleteChannelGroup,
  useRemoveChannelGroupMember,
  useReorderChannelGroupMembers,
  useSetChannelGroupPointer,
  useSetDefaultChannelGroup,
  useUpdateChannelGroup,
} from '../../data/channelGroups';
import { DividedStack } from '../../components/DividedStack';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { AdminConfigSection } from '../../components/admin/AdminConfigWorkspace';
import { useAdminSelectionParam } from '../../hooks/useAdminSelectionParam';
import { Alert } from '../../ui/Alert';
import { Badge, type BadgeTone } from '../../ui/Badge';
import { Button } from '../../ui/Button';
import { Card } from '../../ui/Card';
import { Input } from '../../ui/Input';
import { Modal } from '../../ui/Modal';
import { Select } from '../../ui/Select';
import { Table } from '../../ui/Table';

function statusBadge(status: boolean): { tone: BadgeTone; label: string } {
  if (status) return { tone: 'success', label: '启用' };
  return { tone: 'neutral', label: '禁用' };
}

function memberType(member: AdminChannelGroupMember): 'channel' | 'unknown' {
  if (member.member_channel_id) return 'channel';
  return 'unknown';
}

type ChannelGroupDraft = {
  name: string;
  description: string;
  price_multiplier: number;
  status: boolean;
  type: string;
};

function emptyDraft(): ChannelGroupDraft {
  return {
    name: '',
    description: '',
    price_multiplier: 1,
    status: false,
    type: '',
  };
}

function draftFromGroup(group: AdminChannelGroup): ChannelGroupDraft {
  return {
    name: group.name || '',
    description: group.description || '',
    price_multiplier: group.price_multiplier || 1,
    status: !!group.status,
    type: group.type || '',
  };
}

/** The request body a draft becomes: trimmed, with blank optionals turned to null/undefined. */
function normalizeDraft(draft: ChannelGroupDraft) {
  return {
    name: draft.name.trim(),
    description: draft.description.trim() || null,
    price_multiplier: draft.price_multiplier > 0 ? draft.price_multiplier : undefined,
    status: draft.status,
    type: draft.type.trim(),
  };
}

export function ChannelGroupsPage() {
  const [selectedParam, setSelectedParam] = useAdminSelectionParam('group');
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [addChannelID, setAddChannelID] = useState('');

  const selectedID = (() => {
    const parsed = Number.parseInt(selectedParam, 10);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
  })();

  const groupsQuery = useChannelGroups();
  const groups = groupsQuery.data ?? [];
  const detailQuery = useChannelGroupDetail(selectedID || null);
  const pointerQuery = useChannelGroupPointer(selectedID || null);

  const selectedGroup = detailQuery.data?.group ?? null;
  const members = detailQuery.data?.members ?? [];
  const availableChannels = detailQuery.data?.channels ?? [];
  const pointer = pointerQuery.data ?? null;
  const detailLoading = selectedID > 0 && detailQuery.isPending;

  // The draft is seeded from `selectedGroup` and otherwise lives independently of
  // it, so in-progress edits survive a background refetch. It only needs
  // reseeding when the *selected group itself* changes — tracked by id here
  // rather than through an effect, since this is adjusting state during render
  // (see https://react.dev/learn/you-might-not-need-an-effect), not syncing to a
  // fetch result.
  const [draft, setDraft] = useState<ChannelGroupDraft>(emptyDraft());
  const [draftGroupID, setDraftGroupID] = useState(0);
  if (selectedGroup && draftGroupID !== selectedGroup.id) {
    setDraft(draftFromGroup(selectedGroup));
    setDraftGroupID(selectedGroup.id);
    setAddChannelID('');
  } else if (!selectedGroup && draftGroupID !== 0) {
    setDraft(emptyDraft());
    setDraftGroupID(0);
  }

  const enabledCount = groups.filter((group) => group.status).length;
  const normalizedValue = normalizeDraft(draft);
  const isDirty =
    !!selectedGroup &&
    JSON.stringify(normalizedValue) !== JSON.stringify(normalizeDraft(draftFromGroup(selectedGroup)));
  const saveBlockedReason = !selectedGroup
    ? '未选择渠道组'
    : !normalizedValue.name
      ? '名称不能为空'
      : !isDirty
        ? '没有未保存改动'
        : '';

  const createGroup = useCreateChannelGroup();
  const updateGroup = useUpdateChannelGroup();
  const deleteGroup = useDeleteChannelGroup();
  const setDefaultGroup = useSetDefaultChannelGroup();
  const addMember = useAddChannelGroupMember();
  const removeMember = useRemoveChannelGroupMember();
  const reorderMembers = useReorderChannelGroupMembers();
  const setPointer = useSetChannelGroupPointer();

  function clearBanners() {
    setErr('');
    setNotice('');
  }

  // Clearing the URL param both closes the modal (`open` is derived from it)
  // and discards the in-progress draft: state that only existed for the
  // dialog, dropped rather than reset.
  function closeModal() {
    setSelectedParam(null);
  }

  async function handleStartCreate() {
    clearBanners();
    try {
      const created = await createGroup.mutateAsync({
        name: `channel-group-${Date.now().toString(36)}`,
        price_multiplier: 1,
        status: false,
      });
      setSelectedParam(created.id);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '创建失败');
    }
  }

  async function handleSave() {
    if (!selectedGroup || saveBlockedReason) return;
    clearBanners();
    try {
      await updateGroup.mutateAsync({ groupID: selectedGroup.id, req: normalizedValue });
      setNotice('已保存');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存失败');
    }
  }

  async function handleDelete() {
    if (!selectedGroup) return;
    if (!window.confirm(draft.name.trim() ? `确认删除渠道组 ${draft.name.trim()}？` : '确认删除该渠道组？')) return;
    clearBanners();
    try {
      await deleteGroup.mutateAsync(selectedGroup.id);
      setNotice('已删除');
      closeModal();
    } catch (e) {
      setErr(e instanceof Error ? e.message : '删除失败');
    }
  }

  async function handleSetDefault() {
    if (!selectedGroup) return;
    clearBanners();
    try {
      await setDefaultGroup.mutateAsync(selectedGroup.id);
      setNotice('已设置默认渠道组');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '设置失败');
    }
  }

  async function handleAddMember() {
    if (!selectedGroup) return;
    const channelID = Number.parseInt(addChannelID, 10);
    if (!Number.isFinite(channelID) || channelID <= 0) return;
    clearBanners();
    try {
      await addMember.mutateAsync({ groupID: selectedGroup.id, channelID });
      setAddChannelID('');
      setNotice('已添加渠道');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '添加失败');
    }
  }

  async function handleReorder(index: number, direction: -1 | 1) {
    if (!selectedGroup) return;
    const nextIDs = members.map((item) => item.member_id);
    [nextIDs[index], nextIDs[index + direction]] = [nextIDs[index + direction], nextIDs[index]];
    clearBanners();
    try {
      await reorderMembers.mutateAsync({ groupID: selectedGroup.id, memberIDs: nextIDs });
      setNotice('已更新顺序');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存排序失败');
    }
  }

  async function handleSetPointer(channelID: number) {
    if (!selectedGroup) return;
    if (!window.confirm('确认将该渠道设为该组指针？')) return;
    clearBanners();
    try {
      await setPointer.mutateAsync({ groupID: selectedGroup.id, channelID, pinned: true });
      setNotice('已设置指针');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '设置失败');
    }
  }

  async function handleRemoveMember(member: AdminChannelGroupMember) {
    if (!selectedGroup) return;
    if (!window.confirm('确认从该组移除该成员？')) return;
    clearBanners();
    try {
      if (memberType(member) === 'channel' && member.member_channel_id) {
        await removeMember.mutateAsync({ groupID: selectedGroup.id, channelID: member.member_channel_id });
      } else {
        throw new Error('成员类型不合法');
      }
      setNotice('已移除成员');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '移除失败');
    }
  }

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <Card>
            <div className="d-flex flex-column flex-md-row justify-content-between align-items-center">
              <div className="d-flex align-items-center mb-3 mb-md-0">
                <div
                  className="bg-warning bg-opacity-10 text-warning rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">hub</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">渠道组</h5>
                  <p className="mb-0 text-muted small">
                    {enabledCount} 启用 / {groups.length} 总计
                  </p>
                </div>
              </div>

              <Button
                variant="solid"
                tone="primary"
                size="sm"
                disabled={createGroup.isPending}
                onClick={() => void handleStartCreate()}
              >
                <span className="material-symbols-rounded me-1">add</span>
                {createGroup.isPending ? '创建中…' : '新建渠道组'}
              </Button>
            </div>
          </Card>

          {notice ? (
            <Alert tone="success" icon={<span className="material-symbols-rounded">check_circle</span>}>
              {notice}
            </Alert>
          ) : null}

          {err ? (
            <Alert tone="danger" icon={<span className="material-symbols-rounded">warning</span>}>
              {err}
            </Alert>
          ) : null}

          {groupsQuery.isPending ? (
            <div className="text-muted">加载中…</div>
          ) : groups.length === 0 ? (
            <div className="text-center py-5 text-muted">
              <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
              暂无渠道组。
            </div>
          ) : (
            <Card padding="none">
              <Table
                head={
                  <tr>
                    <th className="ps-4">渠道组</th>
                    <th>倍率</th>
                    <th>描述</th>
                    <th>指针</th>
                    <th>状态</th>
                    <th className="text-end pe-4">操作</th>
                  </tr>
                }
              >
                {groups.map((group) => {
                  const status = statusBadge(group.status);
                  const pointerLabel =
                    typeof group.pointer_channel_id === 'number' && group.pointer_channel_id > 0
                      ? `${group.pointer_channel_name?.trim() || `channel-${group.pointer_channel_id}`}${group.pointer_pinned ? ' · 已固定' : ''}`
                      : '无指针';
                  return (
                    <tr key={group.id}>
                      <td className="ps-4">
                        <div className="d-flex align-items-center gap-2 flex-wrap">
                          <span className="fw-semibold">{group.name}</span>
                          {group.is_default ? <Badge tone="primary">默认</Badge> : null}
                        </div>
                      </td>
                      <td className="text-muted small">x{group.price_multiplier}</td>
                      <td className="text-muted small">{group.description || '无描述'}</td>
                      <td className="text-muted small">{pointerLabel}</td>
                      <td>
                        <Badge tone={status.tone} pill>
                          {status.label}
                        </Badge>
                      </td>
                      <td className="text-end pe-4">
                        <Button variant="soft" size="sm" onClick={() => setSelectedParam(group.id)}>
                          编辑
                        </Button>
                      </td>
                    </tr>
                  );
                })}
              </Table>
            </Card>
          )}
        </DividedStack>
      </SegmentedFrame>

      <Modal
        open={!!selectedParam}
        onClose={closeModal}
        title="渠道组"
        size="lg"
        scrollable
        footer={
          <>
            <div className="me-auto small text-muted">
              {isDirty ? saveBlockedReason || '有未保存改动' : '关闭时未保存改动会直接丢弃。'}
            </div>
            <Button variant="quiet" onClick={closeModal}>
              关闭
            </Button>
            <Button
              variant="solid"
              tone="primary"
              disabled={!!saveBlockedReason || updateGroup.isPending || detailLoading}
              onClick={() => void handleSave()}
            >
              {updateGroup.isPending ? '保存中…' : '保存'}
            </Button>
          </>
        }
      >
        {detailLoading ? (
          <div className="text-muted">加载中…</div>
        ) : !selectedGroup ? (
          <div className="text-muted">未找到该渠道组。</div>
        ) : (
          <>
            <div className="d-flex justify-content-between align-items-start gap-3 flex-wrap mb-3">
              <div className="text-muted small">倍率 x{selectedGroup.price_multiplier}</div>
              <div className="d-flex gap-2 flex-wrap">
                <Button
                  variant="outline"
                  tone="warning"
                  size="sm"
                  disabled={selectedGroup.is_default || !draft.status || setDefaultGroup.isPending}
                  onClick={() => void handleSetDefault()}
                >
                  {selectedGroup.is_default ? '默认渠道组' : '设为默认'}
                </Button>
                <Button
                  variant="outline"
                  tone="danger"
                  size="sm"
                  disabled={deleteGroup.isPending}
                  onClick={() => void handleDelete()}
                >
                  删除渠道组
                </Button>
              </div>
            </div>

            <AdminConfigSection
              id="channel-group-basic"
              title="基本信息"
              description="名称、描述、倍率、默认组和当前指针。"
            >
              <div className="row g-3">
                <div className="col-md-6">
                  <label className="form-label">渠道组名称</label>
                  <Input value={draft.name} onChange={(e) => setDraft((prev) => ({ ...prev, name: e.target.value }))} />
                </div>
                <div className="col-md-6">
                  <label className="form-label">状态</label>
                  <Select
                    value={draft.status ? '1' : '0'}
                    onChange={(e) => setDraft((prev) => ({ ...prev, status: e.target.value === '1' }))}
                  >
                    <option value="1">启用</option>
                    <option value="0">禁用</option>
                  </Select>
                </div>
                <div className="col-md-6">
                  <label className="form-label">价格倍率</label>
                  <div className="input-group">
                    <span className="input-group-text">×</span>
                    <Input
                      type="number"
                      step="any"
                      min="0"
                      value={draft.price_multiplier}
                      onChange={(e) =>
                        setDraft((prev) => ({
                          ...prev,
                          price_multiplier: Number.parseFloat(e.target.value) || 0,
                        }))
                      }
                    />
                  </div>
                </div>
                <div className="col-md-6">
                  <label className="form-label">类型</label>
                  <Input
                    value={draft.type}
                    onChange={(e) => setDraft((prev) => ({ ...prev, type: e.target.value }))}
                    placeholder="可选，自由文本"
                  />
                </div>
                <div className="col-md-6">
                  <label className="form-label">当前指针</label>
                  <Input
                    recessed
                    value={pointer?.channel_name || (pointer?.channel_id ? `channel-${pointer.channel_id}` : '未设置')}
                    disabled
                  />
                </div>
                <div className="col-12">
                  <label className="form-label">描述</label>
                  <Input
                    value={draft.description}
                    onChange={(e) => setDraft((prev) => ({ ...prev, description: e.target.value }))}
                    placeholder="可选"
                  />
                </div>
              </div>
            </AdminConfigSection>

            <AdminConfigSection
              id="channel-group-members"
              title="成员管理"
              description="渠道组只允许直接包含渠道，并维护顺序和指针。"
            >
              <div className="border rounded p-3 mb-4">
                <div className="fw-semibold mb-2">添加渠道</div>
                <div className="mb-2">
                  <Select value={addChannelID} onChange={(e) => setAddChannelID(e.target.value)}>
                    <option value="">选择一个渠道…</option>
                    {availableChannels.map((channel) => (
                      <option key={channel.id} value={String(channel.id)}>
                        {channel.name} · {channel.type}
                      </option>
                    ))}
                  </Select>
                </div>
                <Button
                  variant="outline"
                  tone="primary"
                  size="sm"
                  disabled={!addChannelID}
                  onClick={() => void handleAddMember()}
                >
                  添加到当前组
                </Button>
              </div>

              {members.length === 0 ? (
                <div className="text-muted small">暂无成员，请先添加渠道。</div>
              ) : (
                <Table
                  head={
                    <tr>
                      <th>成员</th>
                      <th>类型</th>
                      <th>状态</th>
                      <th>顺序</th>
                      <th className="text-end">操作</th>
                    </tr>
                  }
                >
                  {members.map((member, index) => {
                    const type = memberType(member);
                    const typeLabel = type === 'channel' ? '渠道' : '未知';
                    const status =
                      type === 'channel' ? statusBadge(!!member.member_channel_status) : statusBadge(false);
                    const isPointerChannel =
                      type === 'channel' && pointer?.pinned && pointer.channel_id === member.member_channel_id;
                    return (
                      <tr key={member.member_id}>
                        <td>
                          <div className="d-flex flex-column">
                            <div className="d-flex align-items-center gap-2 flex-wrap">
                              <span className="fw-semibold">
                                {type === 'channel'
                                  ? member.member_channel_name || `channel-${member.member_channel_id}`
                                  : '-'}
                              </span>
                              {member.promotion ? <Badge tone="warning">优先</Badge> : null}
                              {isPointerChannel ? <Badge tone="warning">指针</Badge> : null}
                            </div>
                            <div className="text-muted small">
                              {type === 'channel'
                                ? `${(member.member_channel_type || '').trim()} · channel ID：${member.member_channel_id || '-'}`
                                : '未知成员'}
                            </div>
                          </div>
                        </td>
                        <td>{typeLabel}</td>
                        <td>
                          <Badge tone={status.tone} pill>
                            {status.label}
                          </Badge>
                        </td>
                        <td>
                          <div className="d-flex gap-1">
                            <Button
                              variant="soft"
                              size="sm"
                              disabled={index === 0}
                              onClick={() => void handleReorder(index, -1)}
                            >
                              上移
                            </Button>
                            <Button
                              variant="soft"
                              size="sm"
                              disabled={index === members.length - 1}
                              onClick={() => void handleReorder(index, 1)}
                            >
                              下移
                            </Button>
                          </div>
                        </td>
                        <td className="text-end">
                          <div className="d-inline-flex gap-1 flex-wrap justify-content-end">
                            {type === 'channel' && member.member_channel_id ? (
                              <Button
                                variant="soft"
                                tone="warning"
                                size="sm"
                                disabled={isPointerChannel}
                                onClick={() => void handleSetPointer(member.member_channel_id as number)}
                              >
                                设为指针
                              </Button>
                            ) : null}
                            <Button
                              variant="soft"
                              tone="danger"
                              size="sm"
                              onClick={() => void handleRemoveMember(member)}
                            >
                              移除
                            </Button>
                          </div>
                        </td>
                      </tr>
                    );
                  })}
                </Table>
              )}
            </AdminConfigSection>
          </>
        )}
      </Modal>
    </div>
  );
}
