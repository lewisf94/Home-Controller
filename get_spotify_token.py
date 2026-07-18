REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPE = (
    "user-modify-playback-state user-read-playback-state "
    "user-read-currently-playing user-library-read"
)


def main() -> int:
    try:
        from spotipy.oauth2 import SpotifyOAuth
    except ImportError:
        print("Install spotipy first:  pip install spotipy")
        return 1

    print("--- CYD Spotify Token Generator ---")
    client_id = input("Enter your Spotify Client ID: ").strip()
    client_secret = input("Enter your Spotify Client Secret: ").strip()

    sp_oauth = SpotifyOAuth(
        client_id=client_id,
        client_secret=client_secret,
        redirect_uri=REDIRECT_URI,
        scope=SCOPE,
    )

    url = sp_oauth.get_authorize_url()
    print(f"\nOpening browser to authorize... If it doesn't open, go here:\n{url}\n")
    print("Scopes requested:", SCOPE)
    print("Note: refresh-token scopes are fixed when authorised. If your old token")
    print("lacks user-library-read, mint and install a fresh token.")

    # This starts a local server, opens the browser, and catches the token.
    token_info = sp_oauth.get_access_token(as_dict=True)

    if token_info and "refresh_token" in token_info:
        print("\n" + "=" * 50)
        print("SUCCESS! Here is your Refresh Token:")
        print("--------------------------------------------------")
        print(token_info["refresh_token"])
        print("--------------------------------------------------")
        print("Paste it into SPOTIFY_REFRESH_TOKEN in your secrets.h")
        print("(secrets.h is gitignored - never put credentials in a tracked")
        print(" file like main.cpp, or they end up published in git history).")
        print("=" * 50 + "\n")
        return 0

    print("\nFailed to get token.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
