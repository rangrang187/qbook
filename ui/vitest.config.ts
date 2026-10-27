import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  resolve: {
    // 只移除构建器的边界标记；真实 server 模块和原生 ZMQ 仍执行。
    alias: { 'server-only': fileURLToPath(new URL('./tests/support/server-only.ts', import.meta.url)) },
  },
  test: {
    include: ['tests/**/*.test.ts', 'tests/**/*.test.tsx'],
    environment: 'node',
    testTimeout: 5000,
    hookTimeout: 10000,
  },
});
