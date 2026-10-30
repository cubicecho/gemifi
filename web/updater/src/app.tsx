import { useState } from 'react';
import { PageLayout } from '@/components/page-layout';
import { Badge } from '@/components/ui/badge';
import { ConnectionCard } from './device/connection-card.tsx';
import { DeviceCard } from './device/device-card.tsx';
import { FlashCard } from './device/flash-card.tsx';
import { LogCard } from './device/log-card.tsx';
import { useConnection } from './device/use-connection.ts';
import { useDeviceLog } from './device/use-device-log.ts';
import { useHello } from './device/use-hello.ts';

/**
 * The updater page: connection, device facts, firmware upload and the log.
 *
 * @returns The page.
 */
export function App() {
  const { access, snapshot, connect } = useConnection();
  const { state: hello, refresh } = useHello(snapshot.client);
  const log = useDeviceLog(snapshot.client);
  const [isFlashing, setIsFlashing] = useState(false);
  const manager = access.kind === 'ready' ? access.manager : null;
  const isConnected = snapshot.selected !== null;

  return (
    <PageLayout
      title="gemifi updater"
      description="Device info, debug log and firmware upload, over Web MIDI."
      actionSlot={
        <Badge variant={isConnected ? 'positive-soft' : 'secondary'}>
          {isConnected ? 'Connected' : 'Not connected'}
        </Badge>
      }
      contentSlot={
        <>
          <ConnectionCard
            access={access}
            snapshot={snapshot}
            onConnect={connect}
            onSelect={(id) => manager?.select(id)}
          />
          <div className="grid min-w-0 gap-4 lg:grid-cols-2">
            <DeviceCard client={snapshot.client} hello={hello} onRefresh={refresh} isBusy={isFlashing} />
            <FlashCard
              link={manager}
              device={hello.kind === 'ok' ? hello.info : null}
              onRunningChange={setIsFlashing}
            />
          </div>
          <LogCard lines={log.lines} rawLines={log.rawLines} onClear={log.clear} />
        </>
      }
    />
  );
}
