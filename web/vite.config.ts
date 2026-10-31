import { defineConfig } from "vite";

// Static output for Cloudflare Pages: `npm run build` -> ../build/web (repo-level build dir)
export default defineConfig({
  base: "./",
  build: { outDir: "../build/web", emptyOutDir: true, target: "es2020", sourcemap: false },
});
