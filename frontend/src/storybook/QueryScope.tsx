import { useState } from 'react';
import type { ReactNode } from 'react';
import { QueryClientProvider } from '@tanstack/react-query';

import { createQueryClient } from '../data/queryClient';

/** Holds one private cache for the subtree, created once per mount. */
export function QueryScope({ children }: { children: ReactNode }) {
  const [client] = useState(createQueryClient);
  return <QueryClientProvider client={client}>{children}</QueryClientProvider>;
}
