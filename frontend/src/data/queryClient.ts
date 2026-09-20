import { QueryClient } from '@tanstack/react-query';

/**
 * Builds the console's cache. A factory rather than a module-level singleton so
 * that an isolated rendering environment (Storybook, tests) can hand every story
 * a private cache instead of leaking entries between them.
 *
 * `retry: false` matches the behavior the console has always had. `unwrap`
 * turns a business failure ("权限不足") into the same rejected promise as a
 * network failure, and retrying the former three times only delays the error
 * message the user needs to see.
 */
export function createQueryClient() {
  return new QueryClient({
    defaultOptions: {
      queries: {
        staleTime: 30_000,
        retry: false,
        refetchOnWindowFocus: false,
      },
    },
  });
}
