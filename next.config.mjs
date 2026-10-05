// Static export for GitHub Pages, served under /livewall.
const basePath = process.env.NODE_ENV === 'production' ? '/livewall' : '';

/** @type {import('next').NextConfig} */
export default {
  output: 'export',
  basePath,
  trailingSlash: true,
  images: { unoptimized: true },
  // The page script builds DOM imperatively and must run exactly once.
  reactStrictMode: false,
  env: { NEXT_PUBLIC_BASE_PATH: basePath },
};
