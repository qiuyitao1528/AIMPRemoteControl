import en from './en.json';
import ru from './ru.json';
import zh from './zh.json';

export const fallbackLanguage = 'en';
export const resources = { en: { translation: en }, ru: { translation: ru }, zh: { translation: zh } } as const;
