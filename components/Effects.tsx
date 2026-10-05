'use client';

import { useEffect } from 'react';
import { startEffects } from '@/lib/motion';

let started = false;

export default function Effects() {
  useEffect(() => {
    if (started) return;
    started = true;
    startEffects();
  }, []);
  return null;
}
