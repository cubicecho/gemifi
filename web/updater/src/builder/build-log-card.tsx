import { CardLayout } from '@/components/card-layout';
import { Alert } from '@/components/ui/alert';
import { CodeBlock } from '@/components/ui/code';
import { CircleCheck, Play } from '@/components/ui/icons';
import { Spinner } from '@/components/ui/spinner';
import type { BuildState } from './use-build.ts';

/** Props of {@link BuildLogCard}. */
export interface BuildLogCardProps {
  /** A build that has started; the idle state draws no card. */
  build: Exclude<BuildState, { kind: 'idle' }>;
}

/**
 * How a build ended, said once it has.
 *
 * @param props - The build.
 * @returns The alert, or nothing while it runs.
 */
function Outcome({ build }: BuildLogCardProps) {
  if (build.kind === 'failed') {
    return <Alert variant="destructive" title="The build could not run" description={build.message} />;
  }
  if (build.kind === 'running') {
    return null;
  }
  if (build.exitCode === 0) {
    return (
      <Alert
        variant="info"
        iconSlot={<CircleCheck />}
        title="Build succeeded"
        description="The images are listed under Built images, ready to download or flash."
      />
    );
  }
  return (
    <Alert
      variant="destructive"
      title={build.exitCode === null ? 'The build stopped without finishing' : `Build failed (exit ${build.exitCode})`}
      description="The log above says why. Fix the YAML or the toolchain, then build again."
    />
  );
}

/** The streamed output of the last build, and how it ended. */
export function BuildLogCard({ build }: BuildLogCardProps) {
  const isRunning = build.kind === 'running';
  return (
    <CardLayout
      title="Build"
      iconSlot={<Play />}
      actionSlot={isRunning ? <Spinner label="Building" /> : undefined}
      contentSlot={
        <>
          <CodeBlock
            title="Build log"
            content={build.log === '' ? 'Starting…' : build.log}
            wrap
            maxHeight="lg"
            follow
          />
          <Outcome build={build} />
        </>
      }
    />
  );
}
