export type SwitchFieldProps = {
  /** The switch's id. On web it is also what the caption's `<label htmlFor>` points at. */
  id: string;
  label: string;
  /**
   * A line under the caption: what the switch does, or why it cannot be changed. On web it is the
   * switch's description (`aria-describedby`), so the reason a switch is disabled is read with it.
   */
  description?: string | undefined;
  checked: boolean;
  onCheckedChange: (checked: boolean) => void;
  /**
   * The switch cannot be changed, and the row fades as a disabled field does: the switch, the
   * caption and the description. `description` is where to say why.
   */
  disabled?: boolean | undefined;
  /**
   * The switch's tooltip, as on `Switch` and `Button`. With `disabled` the switch stays focusable,
   * so a reason given here can still be hovered and tabbed to.
   */
  hint?: string | undefined;
  className?: string | undefined;
  labelClassName?: string | undefined;
};

/**
 * The caption's type, shared so the two halves cannot drift on it. Full ink, as every other field
 * label: a grey caption read as disabled, and as quiet as the description under it.
 */
export const SWITCH_FIELD_LABEL_CLASS = "text-sm text-foreground";

/**
 * The line under it: `FieldDescription`'s type, which it cannot be on the web, where it sits
 * inside the `<label>`. A disabled row fades it with the caption, as a `Field` fades its
 * description (#466); the reason is still the switch's description, and read with it.
 */
export const SWITCH_FIELD_DESCRIPTION_CLASS = "font-normal text-foreground/60 text-sm";

/** The ids the caption and the description carry, from the switch's own. */
export const switchFieldIds = (id: string) => ({
  labelId: `${id}-label`,
  descriptionId: `${id}-description`,
});
