import { useQuery } from '@tanstack/react-query';

import { getBalance, type BillingBalanceResponse } from '../api/billing';
import { unwrap } from './unwrap';

export const billingKeys = {
  all: ['billing'] as const,
};

export function useBalance() {
  return useQuery<BillingBalanceResponse>({
    queryKey: billingKeys.all,
    queryFn: () => unwrap(getBalance(), '加载失败'),
  });
}
