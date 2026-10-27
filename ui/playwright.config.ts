import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./tests",
  testMatch: "monitoring.spec.ts",
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 120_000,
  expect: { timeout: 20_000 },
  reporter: [["list"]],
  use: { browserName: "chromium", headless: true, viewport: { width: 1440, height: 1000 },
    trace: "retain-on-failure", screenshot: "only-on-failure" },
});
