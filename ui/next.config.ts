import type { NextConfig } from 'next';

const config: NextConfig = {
  // 原生绑定由 Node 加载；不得打入浏览器或改成 Edge runtime。
  serverExternalPackages: ['zeromq'],
  experimental: { useTypeScriptCli: true },
};

export default config;
