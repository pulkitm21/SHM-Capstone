import {
  createContext,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from "react";
import {
  getCurrentUser,
  login as loginRequest,
  logout as logoutRequest,
  type AuthUser,
} from "../services/api";

type LoginCredentials = {
  username: string;
  password: string;
};

type AuthContextValue = {
  user: AuthUser | null;
  isAuthenticated: boolean;
  isAdmin: boolean;
  isLoading: boolean;
  login: (credentials: LoginCredentials) => Promise<void>;
  logout: () => Promise<void>;
  refreshAuth: () => Promise<void>;
};

const AuthContext = createContext<AuthContextValue | undefined>(undefined);

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser] = useState<AuthUser | null>(null);
  const [isLoading, setIsLoading] = useState(true);

  async function refreshAuth() {
    try {
      const response = await getCurrentUser();

      if (response.authenticated && response.user) {
        setUser(response.user);
        return;
      }

      setUser(null);
    } catch (error) {
      setUser(null);

      const message = error instanceof Error ? error.message : "";
      if (!message.startsWith("HTTP 401")) {
        console.warn("Failed to refresh auth state.", error);
      }
    }
  }

  /*
    Restore auth state from the backend session cookie on initial app load.
  */
  useEffect(() => {
    async function loadAuthState() {
      try {
        await refreshAuth();
      } finally {
        setIsLoading(false);
      }
    }

    void loadAuthState();
  }, []);

  /*
    Login uses the backend session endpoint, then verifies that the browser
    can send the new httpOnly cookie back through /api/auth/me.
  */
  const login = async ({ username, password }: LoginCredentials) => {
    await loginRequest({
      username: username.trim(),
      password: password.trim(),
    });

    const currentUser = await getCurrentUser();

    if (!currentUser.authenticated || !currentUser.user) {
      throw new Error("Login completed but session verification failed.");
    }

    setUser(currentUser.user);
  };

  /*
    Logout clears the backend session and local auth state.
  */
  const logout = async () => {
    try {
      await logoutRequest();
    } finally {
      setUser(null);
    }
  };

  const value = useMemo(
    () => ({
      user,
      isAuthenticated: user !== null,
      isAdmin: user?.role === "admin",
      isLoading,
      login,
      logout,
      refreshAuth,
    }),
    [user, isLoading]
  );

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuthContext() {
  const context = useContext(AuthContext);

  if (!context) {
    throw new Error("useAuthContext must be used within AuthProvider");
  }

  return context;
}
