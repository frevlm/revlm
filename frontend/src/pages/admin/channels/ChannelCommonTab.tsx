import { useState } from 'react';

import { type Channel } from '../../../api/channels';
import { type AdminChannelGroup } from '../../../api/admin/channelGroups';
import { useCreateChannel, useUpdateChannel } from '../../../data/channels';

import { parseGroupsCSV, toggleGroupsCSV } from './utils';

/**
 * The form's own state: one object, every field in the shape the input needs.
 *
 * `priority`, `price_multiplier` and `config_text` are strings even though the
 * wire types are number / number / object, because a half-typed number and a
 * half-typed JSON object have no valid parsed form. Parsing happens once, at
 * submit, in `validate` — so there is never a moment where the parsed copy and
 * the typed copy disagree.
 */
type ChannelDraft = {
  type: string;
  name: string;
  status: boolean;
  base_url: string;
  api_key: string;
  groups: string;
  priority: string;
  price_multiplier: string;
  config_text: string;
};

function draftFrom(channel?: Channel): ChannelDraft {
  return {
    type: channel?.type || '',
    name: channel?.name || '',
    status: channel ? !!channel.status : true,
    base_url: channel?.base_url || '',
    api_key: channel?.api_key || '',
    groups: channel?.groups || '',
    priority: String(channel?.priority || 0),
    price_multiplier: String(channel?.price_multiplier ?? 1),
    config_text: JSON.stringify(channel?.config_json || {}, null, 2),
  };
}

type ValidatedDraft = {
  type: string;
  name: string;
  status: boolean;
  base_url: string;
  key: string;
  groups: string;
  priority: number;
  price_multiplier: number;
  config_json: Record<string, unknown>;
};

/** Either the reason the draft cannot be sent, or the request body it becomes. */
function validate(draft: ChannelDraft): { error: string } | { body: ValidatedDraft } {
  const type = draft.type.trim();
  const name = draft.name.trim();
  const baseURL = draft.base_url.trim();
  if (!type) return { error: '渠道类型不能为空' };
  if (!name) return { error: '名称不能为空' };
  if (!baseURL) return { error: '接口基础地址不能为空' };

  const priceMultiplier = Number.parseFloat(draft.price_multiplier);
  if (!Number.isFinite(priceMultiplier) || priceMultiplier < 0) {
    return { error: '价格倍率必须是非负数字' };
  }

  let configJSON: Record<string, unknown>;
  try {
    const parsed: unknown = JSON.parse(draft.config_text || '{}');
    if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') throw new Error();
    configJSON = parsed as Record<string, unknown>;
  } catch {
    return { error: '扩展配置必须是 JSON 对象' };
  }

  return {
    body: {
      type,
      name,
      status: draft.status,
      base_url: baseURL,
      key: draft.api_key,
      groups: draft.groups.trim(),
      priority: Number.parseInt(draft.priority, 10) || 0,
      price_multiplier: priceMultiplier,
      config_json: configJSON,
    },
  };
}

type ChannelCommonTabProps = {
  mode?: 'create' | 'edit';
  /** The channel being edited. Absent in create mode. */
  channel?: Channel;
  channelGroups: AdminChannelGroup[];
  /** Fired after a successful create or save, so the caller can close the modal. */
  onDone?: (channelID: number) => void;
};

/**
 * Seeded once per mount from `channel`. Callers switching between channels give
 * this component a `key`, so React remounts it and the draft is re-seeded —
 * which is what "switching the target is a fresh editor" always meant, without
 * an effect to enforce it.
 */
export function ChannelCommonTab({ mode = 'edit', channel, channelGroups, onDone }: ChannelCommonTabProps) {
  const [draft, setDraft] = useState<ChannelDraft>(() => draftFrom(channel));
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [visibleKey, setVisibleKey] = useState(false);
  const [copied, setCopied] = useState(false);

  const createChannel = useCreateChannel();
  const updateChannel = useUpdateChannel();
  const saving = createChannel.isPending || updateChannel.isPending;

  function edit<K extends keyof ChannelDraft>(field: K, value: ChannelDraft[K]) {
    setDraft((prev) => ({ ...prev, [field]: value }));
  }

  async function copyKey() {
    if (!draft.api_key) return;
    await navigator.clipboard.writeText(draft.api_key);
    setCopied(true);
    window.setTimeout(() => setCopied(false), 1500);
  }

  async function save() {
    const checked = validate(draft);
    if ('error' in checked) {
      setErr(checked.error);
      return;
    }
    setErr('');
    setNotice('');
    try {
      if (mode === 'create') {
        const created = await createChannel.mutateAsync(checked.body);
        setNotice('已创建');
        onDone?.(created.id);
        return;
      }
      if (!channel) throw new Error('渠道不存在');
      await updateChannel.mutateAsync({ id: channel.id, ...checked.body });
      setNotice('已保存');
      onDone?.(channel.id);
    } catch (error) {
      setErr(error instanceof Error ? error.message : '保存失败');
    }
  }

  const formKey = channel?.id ?? 'create';
  const disabled = saving;

  return (
    <div className="d-flex flex-column gap-3">
      <div className="card border-0 shadow-sm">
        <div className="card-header bg-white fw-bold py-3">渠道设置</div>
        <div className="card-body">
          <form
            className="row g-3"
            onSubmit={(event) => {
              event.preventDefault();
              void save();
            }}
          >
            {notice ? (
              <div className="col-12">
                <div className="alert alert-success py-2 mb-0">{notice}</div>
              </div>
            ) : null}
            {err ? (
              <div className="col-12">
                <div className="alert alert-danger py-2 mb-0">{err}</div>
              </div>
            ) : null}
            <div className="col-md-5">
              <label className="form-label fw-medium">渠道类型</label>
              <input
                className="form-control font-monospace"
                value={draft.type}
                onChange={(event) => edit('type', event.target.value)}
                placeholder="例如 my_provider"
                disabled={disabled}
                required
              />
              <div className="form-text small text-muted">核心不验证类型；对应插件自己解释它。</div>
            </div>
            <div className="col-md-4">
              <label className="form-label fw-medium">名称</label>
              <input
                className="form-control"
                value={draft.name}
                onChange={(event) => edit('name', event.target.value)}
                disabled={disabled}
                required
              />
            </div>
            <div className="col-md-3">
              <label className="form-label fw-medium">状态</label>
              <select
                className="form-select"
                value={draft.status ? 'true' : 'false'}
                onChange={(event) => edit('status', event.target.value === 'true')}
                disabled={disabled}
              >
                <option value="true">启用</option>
                <option value="false">禁用</option>
              </select>
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">接口基础地址</label>
              <input
                className="form-control font-monospace"
                value={draft.base_url}
                onChange={(event) => edit('base_url', event.target.value)}
                disabled={disabled}
                required
              />
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">API 密钥</label>
              <div className="d-flex flex-column gap-2">
                <input
                  className="form-control font-monospace"
                  type={visibleKey ? 'text' : 'password'}
                  value={draft.api_key}
                  onChange={(event) => edit('api_key', event.target.value)}
                  disabled={disabled}
                  placeholder="sk-..."
                  autoComplete="new-password"
                />
                <div className="d-flex gap-2">
                  <button
                    type="button"
                    className="btn btn-sm btn-light border"
                    disabled={disabled}
                    onClick={() => setVisibleKey((value) => !value)}
                  >
                    {visibleKey ? '隐藏' : '查看'}
                  </button>
                  <button
                    type="button"
                    className="btn btn-sm btn-light border"
                    disabled={disabled || !draft.api_key}
                    onClick={() => {
                      void copyKey().catch(() => {});
                    }}
                  >
                    {copied ? '已复制' : '复制'}
                  </button>
                </div>
              </div>
              <div className="form-text small text-muted">密钥以明文存储；留空表示清除。</div>
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">渠道组设置</label>
              <div className="card p-2" style={{ maxHeight: 260, overflowY: 'auto' }}>
                {channelGroups.length === 0 ? (
                  <div className="text-muted small px-2 py-1">暂无渠道组（请先到“渠道组”创建）。</div>
                ) : (
                  channelGroups.map((group) => {
                    const selected = parseGroupsCSV(draft.groups).includes(group.name);
                    const groupDisabled = !group.status && !selected;
                    return (
                      <div className="form-check" key={group.id}>
                        <input
                          className="form-check-input"
                          type="checkbox"
                          id={`group_edit_${formKey}_${group.name}`}
                          checked={selected}
                          disabled={groupDisabled || disabled}
                          onChange={(event) =>
                            edit('groups', toggleGroupsCSV(draft.groups, group.name, event.target.checked))
                          }
                        />
                        <label className="form-check-label w-100" htmlFor={`group_edit_${formKey}_${group.name}`}>
                          {group.name}{' '}
                          {!group.status ? <span className="badge bg-secondary ms-1 smaller">禁用</span> : null}
                        </label>
                      </div>
                    );
                  })
                )}
              </div>
            </div>
            <div className="col-md-6">
              <label className="form-label fw-medium">优先级</label>
              <input
                className="form-control"
                value={draft.priority}
                onChange={(event) => edit('priority', event.target.value)}
                inputMode="numeric"
                disabled={disabled}
              />
            </div>
            <div className="col-md-6">
              <label className="form-label fw-medium">价格倍率</label>
              <input
                className="form-control"
                value={draft.price_multiplier}
                onChange={(event) => edit('price_multiplier', event.target.value)}
                inputMode="decimal"
                disabled={disabled}
              />
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">扩展配置 JSON</label>
              <textarea
                className="form-control font-monospace"
                rows={7}
                value={draft.config_text}
                onChange={(event) => edit('config_text', event.target.value)}
                disabled={disabled}
                spellCheck={false}
              />
              <div className="form-text small text-muted">这是原样交给插件的配置。插件前端也可以完全替换这张表单。</div>
            </div>
            <div className="col-12 d-flex justify-content-end">
              <button type="submit" className="btn btn-primary px-4" disabled={disabled}>
                {saving ? (mode === 'create' ? '创建中…' : '保存中…') : mode === 'create' ? '创建渠道' : '保存'}
              </button>
            </div>
          </form>
        </div>
      </div>
    </div>
  );
}
