// The four states a device page can be in. Every device's front matter sets
// `status:` to one of these keys. The label is always shown as text, so the
// colour is never the only signal.
export default {
  works: {
    label: "Works",
    description: "gemifi runs on it and has been checked on real hardware.",
  },
  "in-progress": {
    label: "In progress",
    description: "Firmware builds for it, but it hasn't been checked on hardware yet.",
  },
  researched: {
    label: "Researched",
    description: "We've studied the hardware and stock firmware. No gemifi build yet.",
  },
  planned: {
    label: "Planned",
    description: "On the list. Little is known yet.",
  },
};
