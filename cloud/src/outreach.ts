export interface Message {
  readonly to: string;
  readonly from: string;
  readonly body: string;
}

export interface Sender {
  readonly name: string;
  readonly send: (message: Message) => Promise<void>;
  readonly typing: (to: string, from: string) => Promise<void>;
}

export class SendError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "SendError";
  }
}

export const simulated = (log: (line: string) => void): Sender => ({
  name: "simulated",
  send: async (message) => {
    log(`simulated send to ${message.to}: ${message.body.slice(0, 80)}`);
  },
  typing: async (to) => {
    log(`simulated typing indicator to ${to}`);
  },
});

export const sendblue = (apiKey: string, apiSecret: string): Sender => {
  const headers = {
    "sb-api-key-id": apiKey,
    "sb-api-secret-key": apiSecret,
    "content-type": "application/json",
  };
  return {
    name: "sendblue",
    send: async (message) => {
      const response = await fetch("https://api.sendblue.co/api/send-message", {
        method: "POST",
        headers,
        body: JSON.stringify({
          number: message.to,
          content: message.body,
          ...(message.from === "" ? {} : { from_number: message.from }),
        }),
      });
      if (!response.ok) {
        throw new SendError(`sendblue returned ${response.status}: ${await response.text()}`);
      }
    },
    // Best effort: the bubble is iMessage-only and must never fail a reply.
    typing: async (to, from) => {
      await fetch("https://api.sendblue.co/api/send-typing-indicator", {
        method: "POST",
        headers,
        body: JSON.stringify({ number: to, ...(from === "" ? {} : { from_number: from }) }),
      });
    },
  };
};
