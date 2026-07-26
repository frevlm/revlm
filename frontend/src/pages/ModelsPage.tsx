import { useEffect, useMemo, useState } from 'react';

import { listUserModelsDetail, type UserManagedModel } from '../api/models';
import { formatUSDPlain } from '../format/money';
import { providerCachePriceRows } from '../modelPricingDisplay';

function priceCell(value: string) {
  const n = parseFloat(value);
  if (!Number.isFinite(n) || n <= 0) {
    return <span className="rlm-price-empty">—</span>;
  }
  return (
    <span className="rlm-price">
      <span className="rlm-price-cur">$</span>
      {formatUSDPlain(value)}
    </span>
  );
}

export function ModelsPage() {
  const [models, setModels] = useState<UserManagedModel[]>([]);
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState('');
  const [activeOwner, setActiveOwner] = useState('');

  const ownerGroups = useMemo(() => {
    const buckets = new Map<string, UserManagedModel[]>();
    for (const model of models) {
      const ownedBy = (model.owned_by || '').trim() || 'unknown';
      if (!buckets.has(ownedBy)) {
        buckets.set(ownedBy, []);
      }
      buckets.get(ownedBy)!.push({ ...model, owned_by: ownedBy });
    }

    return Array.from(buckets.entries())
      .map(([ownedBy, ownerModels]) => ({
        ownedBy,
        models: ownerModels.slice().sort((a, b) => a.public_id.localeCompare(b.public_id, 'en-US')),
      }))
      .sort((a, b) => {
        if (a.ownedBy === 'unknown' && b.ownedBy !== 'unknown') return 1;
        if (b.ownedBy === 'unknown' && a.ownedBy !== 'unknown') return -1;
        return a.ownedBy.localeCompare(b.ownedBy, 'en-US');
      });
  }, [models]);

  useEffect(() => {
    if (ownerGroups.length === 0) return;
    if (ownerGroups.some((g) => g.ownedBy === activeOwner)) return;
    setActiveOwner(ownerGroups[0].ownedBy);
  }, [ownerGroups, activeOwner]);

  const currentModels = useMemo(() => {
    return ownerGroups.find((g) => g.ownedBy === activeOwner)?.models || [];
  }, [ownerGroups, activeOwner]);

  // 缓存价格列因归属方而异（anthropic 3 列 / openai 1 列），按当前视图动态取并集。
  const cacheColumns = useMemo(() => {
    const seen = new Map<string, string>();
    for (const m of currentModels) {
      for (const row of providerCachePriceRows(m.owned_by, m)) {
        if (!seen.has(row.key)) {
          seen.set(row.key, row.label);
        }
      }
    }
    return Array.from(seen.entries()).map(([key, label]) => ({ key, label }));
  }, [currentModels]);

  useEffect(() => {
    (async () => {
      setErr('');
      setLoading(true);
      try {
        const res = await listUserModelsDetail();
        if (!res.success) {
          throw new Error(res.message || '加载失败');
        }
        setModels(res.data || []);
      } catch (e) {
        setErr(e instanceof Error ? e.message : '加载失败');
      } finally {
        setLoading(false);
      }
    })();
  }, []);

  return (
    <div className="fade-in-up">
      <div className="rlm-page-head">
        <h1>模型</h1>
        <p className="rlm-page-sub">可用模型与计价，单位 USD / 每百万 Tokens。</p>
      </div>

      {err ? (
        <div className="alert alert-danger" role="alert">
          <span className="me-2 material-symbols-rounded">report</span> {err}
        </div>
      ) : null}

      <div className="card mb-0 overflow-hidden">
        <div className="card-header d-flex flex-wrap align-items-center justify-content-between gap-2">
          <div className="rlm-owner-pills">
            {ownerGroups.map((group) => (
              <button
                key={group.ownedBy}
                type="button"
                className={`rlm-owner-pill ${activeOwner === group.ownedBy ? 'active' : ''}`}
                onClick={() => setActiveOwner(group.ownedBy)}
              >
                {group.ownedBy}
                <span className="rlm-owner-pill-count">{group.models.length}</span>
              </button>
            ))}
          </div>
          <span className="text-muted smaller fw-normal">共 {currentModels.length} 个模型</span>
        </div>

        {loading ? (
          <div className="text-center py-5 text-muted">加载中…</div>
        ) : models.length === 0 ? (
          <div className="text-center py-5 text-muted">
            <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
            暂无可用模型，请联系管理员配置模型目录。
          </div>
        ) : currentModels.length === 0 ? (
          <div className="text-center py-5 text-muted">
            <span className="fs-1 d-block mb-3 material-symbols-rounded">folder_off</span>
            当前归属方视图暂无可用模型。
          </div>
        ) : (
          <table className="table rlm-models-table mb-0">
            <thead>
              <tr>
                <th>模型</th>
                <th className="text-end">输入</th>
                <th className="text-end">输出</th>
                {cacheColumns.map((col) => (
                  <th key={col.key} className="text-end">
                    {col.label}
                  </th>
                ))}
                <th className="text-end">状态</th>
              </tr>
            </thead>
            <tbody>
              {currentModels.map((m) => {
                const cacheValues = new Map(providerCachePriceRows(m.owned_by, m).map((row) => [row.key, row.value]));
                return (
                  <tr key={m.public_id}>
                    <td>
                      <div className="d-flex align-items-center gap-2">
                        {m.icon_url ? (
                          <img
                            className="rlm-model-icon"
                            src={m.icon_url}
                            alt={m.owned_by}
                            title={m.owned_by}
                            loading="lazy"
                            onError={(e) => {
                              (e.currentTarget as HTMLImageElement).style.display = 'none';
                            }}
                          />
                        ) : null}
                        <div>
                          <div className="rlm-model-name">{m.public_id}</div>
                          <div className="rlm-model-owner">{m.owned_by}</div>
                        </div>
                      </div>
                    </td>
                    <td className="text-end">{priceCell(m.input_usd_per_1m)}</td>
                    <td className="text-end">{priceCell(m.output_usd_per_1m)}</td>
                    {cacheColumns.map((col) => (
                      <td key={col.key} className="text-end">
                        {priceCell(cacheValues.get(col.key) || '')}
                      </td>
                    ))}
                    <td className="text-end">
                      <span className="rlm-dot-state">可用</span>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </div>
    </div>
  );
}
