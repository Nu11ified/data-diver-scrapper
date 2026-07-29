export interface Message {
  readonly to: string;
  readonly body: string;
}

export interface Sender {
  readonly name: string;
  readonly send: (message: Message) => Promise<void>;
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
});

export const sendblue = (apiKey: string, apiSecret: string): Sender => ({
  name: "sendblue",
  send: async (message) => {
    const response = await fetch("https://api.sendblue.co/api/send-message", {
      method: "POST",
      headers: {
        "sb-api-key-id": apiKey,
        "sb-api-secret-key": apiSecret,
        "content-type": "application/json",
      },
      body: JSON.stringify({ number: message.to, content: message.body }),
    });
    if (!response.ok) {
      throw new SendError(`sendblue returned ${response.status}: ${await response.text()}`);
    }
  },
});
