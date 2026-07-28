import type { Metadata } from "next";
import Link from "next/link";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";
import { Providers } from "@/components/providers";
import { SiteNav } from "@/components/site-nav";
import { EngineStatus } from "@/components/shared/engine-status";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  title: "Data Diver",
  description:
    "Public-record ingestion console: counties, sources, properties and the model behind them.",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html
      lang="en"
      className={`${geistSans.variable} ${geistMono.variable} h-full antialiased`}
    >
      <body className="flex min-h-full flex-col bg-background text-foreground">
        <Providers>
          <header className="sticky top-0 z-40 border-b bg-background/95 backdrop-blur">
            <div className="mx-auto flex h-14 w-full max-w-6xl items-center gap-6 px-4 sm:px-6">
              <Link
                href="/"
                className="flex items-baseline gap-1 text-base font-semibold tracking-tight"
              >
                <span className="text-primary">Data</span>
                <span>Diver</span>
              </Link>
              <SiteNav />
              <div className="ml-auto">
                <EngineStatus />
              </div>
            </div>
          </header>
          <main className="mx-auto w-full max-w-6xl flex-1 px-4 py-8 sm:px-6">
            {children}
          </main>
        </Providers>
      </body>
    </html>
  );
}
