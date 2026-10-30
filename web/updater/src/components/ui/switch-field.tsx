import {
  SWITCH_FIELD_DESCRIPTION_CLASS,
  SWITCH_FIELD_LABEL_CLASS,
  type SwitchFieldProps,
  switchFieldIds,
} from "@/components/ui/switch-field-base";
import { cn } from "@/lib/utils";
import { Label } from "@/components/ui/label";
import { Switch } from "@/components/ui/switch";

/** A switch with its caption on one row, where pressing the caption toggles it. */
export function SwitchField({
  id,
  label,
  description,
  checked,
  onCheckedChange,
  disabled,
  hint,
  className,
  labelClassName,
}: SwitchFieldProps) {
  const { labelId, descriptionId } = switchFieldIds(id);
  const caption = cn(SWITCH_FIELD_LABEL_CLASS, disabled && "opacity-50", labelClassName);
  return (
    <Label
      htmlFor={id}
      data-slot="switch-field"
      className={cn(
        "flex flex-row gap-2",
        description ? "items-start" : "items-center",
        disabled ? "cursor-not-allowed" : "cursor-pointer",
        className,
      )}
    >
      <Switch
        id={id}
        checked={checked}
        onCheckedChange={onCheckedChange}
        disabled={disabled}
        hint={hint}
        // The description sits inside the `<label>` so the whole row stays the hit target, and a
        // label names its control with everything in it. Naming the switch by the caption alone
        // keeps the description out of the name and leaves it to be the description.
        // So is a hint: its text sits inside the `<label>` too, for the switch to be described by.
        aria-labelledby={description || hint ? labelId : undefined}
        aria-describedby={description ? descriptionId : undefined}
      />
      {description ? (
        <span className="flex min-w-0 flex-col gap-1">
          <span id={labelId} className={caption}>
            {label}
          </span>
          <span id={descriptionId} className={SWITCH_FIELD_DESCRIPTION_CLASS}>
            {description}
          </span>
        </span>
      ) : (
        <span id={labelId} className={caption}>
          {label}
        </span>
      )}
    </Label>
  );
}

export type { SwitchFieldProps };
