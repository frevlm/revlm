import { useQuery } from '@tanstack/react-query';

import { listUserModelsDetail, type PluginModel } from '../api/models';
import { unwrap } from './unwrap';

export const modelKeys = {
  all: ['models'] as const,
};

export function useModels() {
  return useQuery<PluginModel[]>({
    queryKey: modelKeys.all,
    queryFn: () => unwrap(listUserModelsDetail(), '加载失败'),
  });
}
