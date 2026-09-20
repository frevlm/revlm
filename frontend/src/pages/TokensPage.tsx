import { useEffect, useRef, useState } from 'react';

import { type TokenChannelGroupOption, type UserToken } from '../api/tokens';
import {
  useCreateToken,
  useDeleteToken,
  useFetchTokenChannel,
  useRevealToken,
  useRevokeToken,
  useRotateToken,
  useSetTokenChannel,
  useTokenChannel,
  useTokenUsageWindow,
  useTokens,
  type TokenUsageRange,
} from '../data/tokens';
import { BootstrapModal } from '../components/BootstrapModal';
import { DividedStack } from '../components/DividedStack';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { closeModalById } from '../components/modal';
import { formatUSDPlain } from '../format/money';
import { formatLocalDate, formatLocalDateTimeMinute } from './usage/usageUtils';

const emptyUsageRange: TokenUsageRange = { start: '', end: '' };

export function TokensPage() {
  const tokensQuery = useTokens();
  const tokens = tokensQuery.data ?? [];

  const createTokenMutation = useCreateToken();
  const rotateTokenMutation = useRotateToken();
  const revokeTokenMutation = useRevokeToken();
  const deleteTokenMutation = useDeleteToken();
  const revealTokenMutation = useRevealToken();
  const fetchTokenChannel = useFetchTokenChannel();
  const setTokenChannelMutation = useSetTokenChannel();

  // Action errors (reveal/copy/rotate/revoke/delete/create) share one banner with
  // the list's load error, mirroring the single `tokensErr` field this page used
  // to keep for both — whichever fired most recently is what the user sees.
  const [actionErr, setActionErr] = useState('');
  const [revealed, setRevealed] = useState<Record<number, string>>({});
  const [copiedID, setCopiedID] = useState<number | null>(null);
  const [groupNameByID, setGroupNameByID] = useState<Record<number, string>>({});

  const [name, setName] = useState('');

  const openGeneratedTokenModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const pendingGeneratedTokenRef = useRef<string | null>(null);
  const [generatedToken, setGeneratedToken] = useState('');
  const [generatedCopied, setGeneratedCopied] = useState(false);

  const openTokenChannelModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [tokenChannelToken, setTokenChannelToken] = useState<UserToken | null>(null);
  const [selectedGroupID, setSelectedGroupID] = useState(0);
  const [channelErr, setChannelErr] = useState('');
  const [channelNotice, setChannelNotice] = useState('');
  const channelQuery = useTokenChannel(tokenChannelToken?.id ?? null);

  const openTokenUsageModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [usageToken, setUsageToken] = useState<UserToken | null>(null);
  // What is typed, and what has been submitted. The query is keyed on the
  // latter, so editing a date does not fire a request until 查询 is pressed.
  const [usageRange, setUsageRange] = useState<TokenUsageRange>(emptyUsageRange);
  const [usageQueryRange, setUsageQueryRange] = useState<TokenUsageRange>(emptyUsageRange);
  const usageQuery = useTokenUsageWindow(usageToken?.id ?? null, usageQueryRange);

  const tokensErr = actionErr || (tokensQuery.error ? tokensQuery.error.message : '');

  useEffect(() => {
    if (copiedID == null) return;
    const t = window.setTimeout(() => setCopiedID(null), 2000);
    return () => window.clearTimeout(t);
  }, [copiedID]);

  useEffect(() => {
    if (!generatedCopied) return;
    const t = window.setTimeout(() => setGeneratedCopied(false), 2000);
    return () => window.clearTimeout(t);
  }, [generatedCopied]);

  function rememberGroupNames(groups: TokenChannelGroupOption[]) {
    if (!groups.length) return;
    setGroupNameByID((prev) => {
      const next = { ...prev };
      for (const g of groups) {
        if (g.id > 0 && g.name) next[g.id] = g.name;
      }
      return next;
    });
  }

  // Whichever query resolves a token's allowed groups feeds the name lookup
  // the main table uses, since the list endpoint itself only carries an id.
  useEffect(() => {
    if (channelQuery.data?.allowed_channel_groups) rememberGroupNames(channelQuery.data.allowed_channel_groups);
  }, [channelQuery.data]);

  function formatTokenChannelGroup(t: UserToken): string {
    const id = t.channel_group_id || 0;
    if (id <= 0) return '-';
    return groupNameByID[id] || `渠道组 #${id}`;
  }

  function openGeneratedTokenModal(tok: string) {
    setGeneratedCopied(false);
    setGeneratedToken(tok);
    window.setTimeout(() => openGeneratedTokenModalBtnRef.current?.click(), 0);
  }

  async function copyText(raw: string): Promise<boolean> {
    try {
      await navigator.clipboard.writeText(raw);
      return true;
    } catch {
      // fallback
    }
    try {
      const el = document.createElement('textarea');
      el.value = raw;
      el.setAttribute('readonly', 'true');
      el.style.position = 'fixed';
      el.style.top = '0';
      el.style.left = '0';
      el.style.opacity = '0';
      document.body.appendChild(el);
      el.select();
      const ok = document.execCommand('copy');
      document.body.removeChild(el);
      return ok;
    } catch {
      return false;
    }
  }

  async function copyToken(raw: string, tokenID: number) {
    const ok = await copyText(raw);
    if (ok) setCopiedID(tokenID);
  }

  async function revealToken(tokenID: number): Promise<string> {
    if (revealed[tokenID]) return revealed[tokenID];
    const tok = await revealTokenMutation.mutateAsync(tokenID);
    setRevealed((prev) => ({ ...prev, [tokenID]: tok }));
    return tok;
  }

  async function saveTokenChannel() {
    const tokenID = tokenChannelToken?.id || 0;
    if (!tokenID) {
      setChannelErr('未选择 Token');
      setChannelNotice('');
      return;
    }
    if (!selectedGroupID) {
      setChannelErr('请选择一个渠道组');
      setChannelNotice('');
      return;
    }
    setChannelErr('');
    setChannelNotice('');
    try {
      await setTokenChannelMutation.mutateAsync({ tokenID, channelGroupID: selectedGroupID });
      setChannelNotice('已保存');
    } catch (e) {
      setChannelErr(e instanceof Error ? e.message : '保存失败');
    }
  }

  function openTokenUsageModal(t: UserToken) {
    setActionErr('');
    setUsageToken(t);
    setUsageRange(emptyUsageRange);
    setUsageQueryRange(emptyUsageRange);
    window.setTimeout(() => openTokenUsageModalBtnRef.current?.click(), 0);
  }

  function openTokenChannelModal(t: UserToken) {
    setActionErr('');
    setChannelErr('');
    setChannelNotice('');
    setTokenChannelToken(t);
    setSelectedGroupID(t.channel_group_id || 0);
    void (async () => {
      try {
        const data = await fetchTokenChannel(t.id);
        setSelectedGroupID(data.channel_group_id || 0);
      } catch (e) {
        setChannelErr(e instanceof Error ? e.message : '加载失败');
      } finally {
        window.setTimeout(() => openTokenChannelModalBtnRef.current?.click(), 0);
      }
    })();
  }

  const allowedGroups = (channelQuery.data?.allowed_channel_groups || [])
    .slice()
    .sort((a, b) => a.name.localeCompare(b.name, 'zh-CN'));
  const selectedGroup = allowedGroups.find((g) => g.id === selectedGroupID) || null;
  const usageWindow = usageQuery.data?.windows?.[0] ?? null;
  const usageErr = usageQuery.error ? usageQuery.error.message : '';
  // The server's chosen window, shown in the inputs whenever the user has not
  // named one. Displayed, never written back into `usageRange`.
  const usageEchoDay = usageWindow ? formatLocalDate(String(usageWindow.since)) : '';
  const usageStartText = usageRange.start || usageEchoDay;
  const usageEndText = usageRange.end || usageRange.start || usageEchoDay;

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <div className="card mb-0">
            <div className="card-body d-flex flex-column flex-md-row justify-content-between align-items-center">
              <div className="d-flex align-items-center mb-3 mb-md-0">
                <div
                  className="bg-primary bg-opacity-10 text-primary rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">key</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">我的 API 令牌</h5>
                  <p className="mb-0 text-muted small">
                    为安全起见，令牌默认隐藏；可在此页查看/复制。令牌撤销后无法查看。
                  </p>
                </div>
              </div>
              <button
                type="button"
                className="btn btn-primary btn-sm"
                data-bs-toggle="modal"
                data-bs-target="#createTokenModal"
              >
                <span className="me-1 material-symbols-rounded">add</span> 创建令牌
              </button>
            </div>
          </div>

          {tokensErr ? (
            <div className="alert alert-danger mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">report</span>
              {tokensErr}
            </div>
          ) : null}

          <div className="card h-100 overflow-hidden mb-0">
            <div className="card-body p-0">
              <div className="table-responsive">
                <table className="table table-hover align-middle mb-0">
                  <thead className="bg-light text-muted small text-uppercase">
                    <tr>
                      <th scope="col" className="fw-medium ps-4 py-3">
                        名称
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        渠道组
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        预览
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        状态
                      </th>
                      <th scope="col" className="fw-medium text-end pe-4 py-3">
                        操作
                      </th>
                    </tr>
                  </thead>
                  <tbody className="border-top-0">
                    {tokensQuery.isFetching ? (
                      <tr>
                        <td colSpan={5} className="text-center py-5 text-muted">
                          加载中…
                        </td>
                      </tr>
                    ) : tokens.length === 0 ? (
                      <tr>
                        <td colSpan={5} className="text-center py-5 text-muted">
                          <div className="mb-2">
                            <span className="fs-3 text-light-emphasis material-symbols-rounded">inbox</span>
                          </div>
                          暂无令牌，点击右上角按钮创建一个。
                        </td>
                      </tr>
                    ) : (
                      tokens.map((t) => (
                        <tr key={t.id}>
                          <td className="ps-4 py-3">
                            {t.name ? (
                              <span className="fw-medium text-dark">{t.name}</span>
                            ) : (
                              <span className="text-muted small fst-italic">无备注</span>
                            )}
                          </td>
                          <td className="py-3">
                            {t.channel_group_id ? (
                              <span className="small text-dark">{formatTokenChannelGroup(t)}</span>
                            ) : (
                              <span className="text-muted small">-</span>
                            )}
                          </td>
                          <td className="py-3">
                            {revealed[t.id] ? (
                              <code className="bg-light px-2 py-1 rounded text-dark border user-select-all">
                                {revealed[t.id]}
                              </code>
                            ) : (
                              <span className="text-muted small">-</span>
                            )}
                          </td>
                          <td className="py-3">
                            {t.status === 1 ? (
                              <span className="badge bg-success bg-opacity-10 text-success rounded-pill px-2">
                                活跃
                              </span>
                            ) : (
                              <span className="badge bg-secondary bg-opacity-10 text-secondary rounded-pill px-2">
                                已撤销
                              </span>
                            )}
                          </td>
                          <td className="text-end pe-4 py-3">
                            {t.status === 1 ? (
                              <>
                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={
                                    tokensQuery.isFetching ||
                                    (revealTokenMutation.isPending && revealTokenMutation.variables === t.id)
                                  }
                                  onClick={async () => {
                                    setActionErr('');
                                    if (revealed[t.id]) {
                                      setRevealed((prev) => {
                                        const next = { ...prev };
                                        delete next[t.id];
                                        return next;
                                      });
                                      return;
                                    }
                                    try {
                                      await revealToken(t.id);
                                    } catch (e) {
                                      setActionErr(e instanceof Error ? e.message : '查看失败');
                                    }
                                  }}
                                >
                                  {revealed[t.id] ? '隐藏' : '查看'}
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={
                                    tokensQuery.isFetching ||
                                    (revealTokenMutation.isPending && revealTokenMutation.variables === t.id)
                                  }
                                  onClick={async () => {
                                    setActionErr('');
                                    try {
                                      const tok = revealed[t.id] ? revealed[t.id] : await revealToken(t.id);
                                      await copyToken(tok, t.id);
                                    } catch (e) {
                                      setActionErr(e instanceof Error ? e.message : '复制失败');
                                    }
                                  }}
                                >
                                  {copiedID === t.id ? '已复制' : '复制'}
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensQuery.isFetching}
                                  onClick={() => openTokenChannelModal(t)}
                                >
                                  渠道组
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensQuery.isFetching}
                                  onClick={() => openTokenUsageModal(t)}
                                >
                                  用量
                                </button>

                                <span className="text-muted small mx-2">|</span>
                              </>
                            ) : null}

                            <button
                              className="btn btn-link text-primary p-0 text-decoration-none small"
                              type="button"
                              disabled={tokensQuery.isFetching}
                              onClick={async () => {
                                setActionErr('');
                                setRevealed((prev) => {
                                  const next = { ...prev };
                                  delete next[t.id];
                                  return next;
                                });
                                try {
                                  const created = await rotateTokenMutation.mutateAsync(t.id);
                                  if (created.token) openGeneratedTokenModal(created.token);
                                } catch (e) {
                                  setActionErr(e instanceof Error ? e.message : '重新生成失败');
                                }
                              }}
                            >
                              重新生成
                            </button>

                            <span className="text-muted small mx-2">|</span>

                            {t.status === 1 ? (
                              <button
                                className="btn btn-link text-danger p-0 text-decoration-none small"
                                type="button"
                                disabled={tokensQuery.isFetching}
                                onClick={async () => {
                                  setActionErr('');
                                  try {
                                    await revokeTokenMutation.mutateAsync(t.id);
                                  } catch (e) {
                                    setActionErr(e instanceof Error ? e.message : '撤销失败');
                                  }
                                }}
                              >
                                撤销
                              </button>
                            ) : (
                              <button
                                className="btn btn-link text-danger p-0 text-decoration-none small"
                                type="button"
                                disabled={tokensQuery.isFetching}
                                onClick={async () => {
                                  setActionErr('');
                                  try {
                                    await deleteTokenMutation.mutateAsync(t.id);
                                  } catch (e) {
                                    setActionErr(e instanceof Error ? e.message : '删除失败');
                                  }
                                }}
                              >
                                删除
                              </button>
                            )}
                          </td>
                        </tr>
                      ))
                    )}
                  </tbody>
                </table>
              </div>
            </div>
          </div>
        </DividedStack>
      </SegmentedFrame>

      {/* programmatically open the generated-token modal */}
      <button
        ref={openGeneratedTokenModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#generatedTokenModal"
      ></button>

      {/* programmatically open the token-channel modal */}
      <button
        ref={openTokenChannelModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#tokenChannelModal"
      ></button>

      {/* programmatically open the token-usage modal */}
      <button
        ref={openTokenUsageModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#tokenUsageModal"
      ></button>

      <BootstrapModal
        id="tokenUsageModal"
        title="令牌用量"
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setUsageToken(null);
          setUsageRange(emptyUsageRange);
          setUsageQueryRange(emptyUsageRange);
        }}
      >
        {usageToken ? (
          <div>
            <div className="mb-3">
              <div className="text-muted small mb-1">令牌</div>
              <div className="fw-semibold">{(usageToken.name || `Token #${usageToken.id}`).toString()}</div>
            </div>

            {usageErr ? (
              <div className="alert alert-danger mb-3" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                {usageErr}
              </div>
            ) : null}

            <form
              className="row g-2 align-items-end mb-3"
              onSubmit={(e) => {
                e.preventDefault();
                setUsageQueryRange({ start: usageRange.start.trim(), end: usageRange.end.trim() });
              }}
            >
              <div className="col-auto">
                <label className="form-label small text-muted mb-1">开始日期</label>
                <input
                  className="form-control form-control-sm"
                  type="date"
                  value={usageStartText}
                  onChange={(e) => setUsageRange((prev) => ({ ...prev, start: e.target.value }))}
                  disabled={usageQuery.isFetching}
                />
              </div>
              <div className="col-auto">
                <label className="form-label small text-muted mb-1">结束日期</label>
                <input
                  className="form-control form-control-sm"
                  type="date"
                  value={usageEndText}
                  onChange={(e) => setUsageRange((prev) => ({ ...prev, end: e.target.value }))}
                  disabled={usageQuery.isFetching}
                />
              </div>
              <div className="col-auto d-flex gap-2">
                <button className="btn btn-sm btn-primary" type="submit" disabled={usageQuery.isFetching}>
                  查询
                </button>
                <button
                  className="btn btn-sm btn-white border text-dark"
                  type="button"
                  disabled={usageQuery.isFetching}
                  onClick={() => {
                    setUsageRange(emptyUsageRange);
                    setUsageQueryRange(emptyUsageRange);
                  }}
                >
                  重置
                </button>
              </div>
            </form>

            {usageQuery.isFetching ? <div className="text-muted">加载中…</div> : null}

            {usageWindow ? (
              <div className="table-responsive">
                <table className="table table-sm mb-0">
                  <tbody>
                    <tr>
                      <td className="text-muted">时间范围</td>
                      <td className="text-end">
                        {formatLocalDateTimeMinute(String(usageWindow.since))} -{' '}
                        {formatLocalDateTimeMinute(String(usageWindow.until))}
                      </td>
                    </tr>
                    <tr>
                      <td className="text-muted">消耗 (USD)</td>
                      <td className="text-end">{formatUSDPlain(usageWindow.usd)}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">请求数</td>
                      <td className="text-end">{usageWindow.requests}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">平均首字延迟</td>
                      <td className="text-end">{usageWindow.avg_first_token_latency}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">RPM</td>
                      <td className="text-end">{usageWindow.rpm}</td>
                    </tr>
                  </tbody>
                </table>
              </div>
            ) : null}
          </div>
        ) : (
          <div className="text-muted">请选择一个令牌。</div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="generatedTokenModal"
        title="令牌已生成"
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setGeneratedCopied(false);
          setGeneratedToken('');
        }}
      >
        <div className="alert alert-warning border-0 bg-warning bg-opacity-10 d-flex align-items-start mb-3">
          <span className="me-2 mt-1 material-symbols-rounded">warning</span>
          <div className="small">请复制并妥善保存。也可以在令牌列表页查看/复制（默认隐藏）。令牌撤销后无法查看。</div>
        </div>

        <div className="mb-3">
          <label className="form-label small fw-bold text-uppercase text-muted">API 令牌</label>
          <div className="input-group input-group-lg">
            <input
              type="text"
              className="form-control font-monospace bg-light border-end-0"
              value={generatedToken}
              readOnly
              onClick={(e) => {
                try {
                  e.currentTarget.select();
                } catch {
                  // ignore
                }
              }}
            />
            <button
              className={`btn ${generatedCopied ? 'btn-success text-white' : 'btn-light'} border border-start-0 px-4`}
              type="button"
              title="点击复制"
              disabled={generatedToken.trim() === ''}
              onClick={async () => {
                setActionErr('');
                const ok = await copyText(generatedToken);
                if (!ok) {
                  setActionErr('复制失败');
                  return;
                }
                setGeneratedCopied(true);
              }}
            >
              <span className="material-symbols-rounded">{generatedCopied ? 'check' : 'content_copy'}</span>
            </button>
          </div>
          <div
            className={`text-success small mt-2 opacity-0 transition-opacity${generatedCopied ? ' opacity-100' : ''}`}
          >
            <span className="me-1 material-symbols-rounded">check</span>已成功复制到剪贴板
          </div>
        </div>

        <div className="modal-footer border-top-0 px-0 pb-0">
          <button type="button" className="btn btn-light" data-bs-dismiss="modal">
            关闭
          </button>
        </div>
      </BootstrapModal>

      <BootstrapModal
        id="tokenChannelModal"
        title={
          tokenChannelToken
            ? `渠道组：${(tokenChannelToken.name || '').trim() || `Token #${tokenChannelToken.id}`}`
            : '渠道组'
        }
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setTokenChannelToken(null);
          setSelectedGroupID(0);
          setChannelErr('');
          setChannelNotice('');
        }}
      >
        {!tokenChannelToken ? (
          <div className="text-muted">未选择 Token。</div>
        ) : (
          <div>
            {channelErr ? (
              <div className="alert alert-danger d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                <div>{channelErr}</div>
              </div>
            ) : null}

            {channelNotice ? (
              <div className="alert alert-success d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">check_circle</span>
                <div>{channelNotice}</div>
              </div>
            ) : null}

            <p className="text-muted small mb-3">为该令牌指定一个渠道组。上游失败时会在组内按顺序切换渠道。</p>

            {channelQuery.isFetching ? <div className="text-muted small mb-2">加载中…</div> : null}

            <div className="mb-3">
              <label className="form-label fw-medium text-dark">渠道组</label>
              <select
                className="form-select"
                value={selectedGroupID || ''}
                onChange={(e) => setSelectedGroupID(Number(e.target.value) || 0)}
                disabled={channelQuery.isFetching || setTokenChannelMutation.isPending}
              >
                <option value="">选择渠道组…</option>
                {allowedGroups.map((g) => (
                  <option key={g.id} value={g.id} disabled={!g.status}>
                    {g.name}
                    {g.price_multiplier ? ` · x${g.price_multiplier}` : ''}
                    {!g.status ? '（禁用）' : ''}
                  </option>
                ))}
              </select>
              {selectedGroup ? (
                <div className="form-text text-muted">
                  {selectedGroup.description ? `${selectedGroup.description} · ` : ''}
                  {selectedGroup.price_multiplier ? `倍率 x${selectedGroup.price_multiplier}` : '倍率 x1'}
                  {!selectedGroup.status ? ' · 当前禁用' : ''}
                </div>
              ) : (
                <div className="form-text text-muted">请从可用渠道组中选择一个。</div>
              )}
            </div>

            <div className="d-grid d-md-flex justify-content-md-end gap-2">
              <button
                type="button"
                className="btn btn-light"
                data-bs-dismiss="modal"
                disabled={setTokenChannelMutation.isPending}
              >
                关闭
              </button>
              <button
                type="button"
                className="btn btn-primary px-4"
                disabled={setTokenChannelMutation.isPending || channelQuery.isFetching || !selectedGroupID}
                onClick={() => void saveTokenChannel()}
              >
                {setTokenChannelMutation.isPending ? '保存中…' : '保存'}
              </button>
            </div>
          </div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="createTokenModal"
        title="创建新 API 令牌"
        dialogClassName="modal-dialog-centered"
        headerClassName="border-bottom-0 pb-0"
        bodyClassName="pt-4"
        onHidden={() => {
          setName('');
          const tok = pendingGeneratedTokenRef.current;
          pendingGeneratedTokenRef.current = null;
          if (tok) openGeneratedTokenModal(tok);
        }}
      >
        <form
          onSubmit={async (e) => {
            e.preventDefault();
            setActionErr('');
            try {
              const created = await createTokenMutation.mutateAsync(name.trim() || undefined);
              pendingGeneratedTokenRef.current = created.token.trim() === '' ? null : created.token;
              setName('');
              closeModalById('createTokenModal');
            } catch (e) {
              setActionErr(e instanceof Error ? e.message : '创建失败');
            }
          }}
        >
          <div className="mb-3">
            <label className="form-label fw-medium text-dark">备注名称</label>
            <input
              name="name"
              type="text"
              className="form-control"
              placeholder="例如：我的项目、笔记本 CLI…"
              autoFocus
              value={name}
              onChange={(e) => setName(e.target.value)}
            />
            <div className="form-text text-muted">给令牌起个名字，方便日后管理。</div>
          </div>
          <div className="alert alert-light border mb-0 d-flex align-items-start small">
            <span className="text-primary me-2 mt-1 material-symbols-rounded">info</span>
            <div>创建成功后会弹窗展示一次；也可以在列表页查看/复制（默认隐藏）。令牌撤销后无法查看。</div>
          </div>
          <div className="modal-footer border-top-0 px-0">
            <button type="button" className="btn btn-light text-muted" data-bs-dismiss="modal">
              取消
            </button>
            <button type="submit" className="btn btn-primary px-4" disabled={tokensQuery.isFetching}>
              创建
            </button>
          </div>
        </form>
      </BootstrapModal>
    </div>
  );
}
