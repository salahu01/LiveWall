import type { Metadata } from 'next';
import { Instrument_Serif, JetBrains_Mono, Space_Grotesk } from 'next/font/google';
import { asset } from '@/lib/site';
import './globals.css';

const sans = Space_Grotesk({ subsets: ['latin'], weight: ['300', '400', '500', '700'], variable: '--font-sans' });
const mono = JetBrains_Mono({ subsets: ['latin'], weight: ['400', '600'], variable: '--font-mono' });
const serif = Instrument_Serif({ subsets: ['latin'], weight: '400', style: ['normal', 'italic'], variable: '--font-serif' });

export const metadata: Metadata = {
  metadataBase: new URL('https://salahu01.github.io'),
  title: "LiveWall — a wallpaper that costs nothing when you aren't looking",
  description:
    'LiveWall: a live wallpaper app for macOS, Windows, Linux and Android that tears its decoder down whenever nothing can see the desktop. 504 KB binary, 12 MB idle, 0.0% CPU when covered.',
  icons: { icon: asset('icon.png') },
  openGraph: {
    title: 'LiveWall',
    description: "A live wallpaper that costs almost nothing when you aren't looking at it.",
    images: ['/livewall/media/aurora-poster.jpg'],
  },
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" className={`${sans.variable} ${mono.variable} ${serif.variable}`}>
      <body>{children}</body>
    </html>
  );
}
