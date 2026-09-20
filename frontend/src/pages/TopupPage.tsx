import { useBalance } from '../data/billing';
import { DividedStack } from '../components/DividedStack';
import { SegmentedFrame } from '../components/SegmentedFrame';

export function TopupPage() {
  const { data, isPending, error } = useBalance();
  const err = error ? error.message : '';

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          {err ? (
            <div className="alert alert-danger d-flex align-items-center mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">warning</span>
              <div>{err}</div>
            </div>
          ) : null}

          <div>
            <div className="d-flex align-items-center mb-3">
              <h4 className="mb-0 fw-bold">余额</h4>
            </div>

            <div className="card border-0 mb-0">
              <div className="card-body p-4">
                <div className="display-6 fw-bold text-dark">{isPending ? '…' : (data?.balance_usd ?? '-')}</div>
                <div className="text-muted small mt-1">余额用于模型调用的按量计费扣费。如需充值请联系管理员。</div>
              </div>
            </div>
          </div>
        </DividedStack>
      </SegmentedFrame>
    </div>
  );
}
