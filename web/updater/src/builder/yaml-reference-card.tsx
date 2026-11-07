import { CardLayout } from '@/components/card-layout';
import { Collapsible } from '@/components/collapsible';
import { Badge } from '@/components/ui/badge';
import { CodeBlock } from '@/components/ui/code';
import { CopyButton } from '@/components/ui/copy-button';
import { Library } from '@/components/ui/icons';
import { YAML_REFERENCE, type YamlReferenceEntry } from './yaml-reference.ts';

/**
 * @param props - One feature's example.
 * @returns Its notes, the settings it adds, and the YAML to copy.
 */
function EntryBody({ entry }: { entry: YamlReferenceEntry }) {
  return (
    <div className="flex min-w-0 flex-col gap-3 pt-2 pl-6">
      <ul className="list-disc space-y-1 pl-4 text-foreground/80 text-sm">
        {entry.notes.map((note) => (
          <li key={note}>{note}</li>
        ))}
      </ul>
      {entry.params.length === 0 ? null : (
        <p className="flex flex-wrap items-center gap-1.5 text-foreground/60 text-sm">
          <span>Live settings:</span>
          {entry.params.map((name) => (
            <Badge key={name} variant="outline" mono>
              {name}
            </Badge>
          ))}
        </p>
      )}
      <CodeBlock
        title={entry.title}
        language="yaml"
        content={entry.yaml}
        actionSlot={<CopyButton value={entry.yaml} label={`Copy the ${entry.title} example`} />}
      />
    </div>
  );
}

/**
 * Examples of the Device YAML features that are easy to get wrong by hand: XY pads, DIN MIDI, the
 * DMA switches, pitch wheels and aftertouch, LED looks and presets. Each one copies, to paste into
 * the editor above.
 */
export function YamlReferenceCard() {
  return (
    <CardLayout
      title="YAML examples"
      iconSlot={<Library />}
      description="Copy one into the editor and change the pins and ids to your board's. Validate says what the builder made of it."
      contentSlot={
        <div className="flex min-w-0 flex-col gap-4">
          {YAML_REFERENCE.map((entry) => (
            <Collapsible
              key={entry.id}
              title={entry.title}
              titleClassName="text-foreground"
              description={entry.summary}
              contentSlot={<EntryBody entry={entry} />}
            />
          ))}
        </div>
      }
    />
  );
}
