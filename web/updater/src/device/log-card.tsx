import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Button } from '@/components/ui/button';
import { CodeBlock } from '@/components/ui/code';
import { FileText } from '@/components/ui/icons';
import { SwitchField } from '@/components/ui/switch-field';

/** Props of {@link LogCard}. */
export interface LogCardProps {
  lines: readonly string[];
  rawLines: readonly string[];
  onClear: () => void;
}

/**
 * The device's LOG and EVENT messages, or every SysEx message as hex.
 *
 * @param props - The lines and a clear handler.
 * @returns The card.
 */
export function LogCard({ lines, rawLines, onClear }: LogCardProps) {
  const [isRaw, setIsRaw] = useState(false);
  const shown = isRaw ? rawLines : lines;
  const empty = isRaw ? 'No SysEx sent or received yet.' : 'No LOG or EVENT messages yet.';
  return (
    <CardLayout
      title="Log"
      iconSlot={<FileText />}
      actionSlot={
        <>
          <SwitchField id="raw-sysex" label="Raw SysEx" checked={isRaw} onCheckedChange={setIsRaw} />
          <Button size="sm" variant="outline" content="Clear" onClick={onClear} />
        </>
      }
      contentSlot={<CodeBlock content={shown.length === 0 ? empty : shown.join('\n')} wrap maxHeight="lg" follow />}
    />
  );
}
